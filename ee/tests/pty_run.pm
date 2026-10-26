#!/usr/bin/env perl

# pty_run.pm - run a program on a pseudo-terminal, feed it key
# sequences, and return (or print) its output.
#
# Requires Perl 5 with the IO::Pty module; on OpenBSD install it with
# "pkg_add p5-IO-Tty" (devel/p5-IO-TTY).  This helper is used by the
# ee behavioural tests only; the other OpenUtils test suites are
# POSIX shell.
#
# usage: pty_run.pm <program> <args...> -- <input-sequence>...
#
# Each input sequence may contain \xHH escapes (or plain text) and is
# sent to the program with a small delay between sequences, ending
# with a final drain period.

use strict;
use warnings;

use Carp   ();
use Encode ();
use IO::Pty;
use IO::Select;
use POSIX       ();
use Time::HiRes qw(time);

POSIX::sigaction( POSIX::SIGPIPE(), POSIX::SigAction->new('IGNORE') );

package Session;

# Spawn the requested program on a new pseudo-terminal.  The parent keeps the
# master side and the child gets the slave as its controlling terminal and
# standard streams, so ncurses sees a real TTY.
sub new {
    my ( $class, $argv, $env ) = @_;
    my $pty = IO::Pty->new or Carp::croak "cannot allocate a pty: $!";
    my $pid = fork;
    Carp::croak "cannot fork: $!" unless defined $pid;
    if ( $pid == 0 ) {
        my $slave = $pty->slave;
        $pty->make_slave_controlling_terminal;
        $pty->close;
        open STDIN,  '<&', $slave or POSIX::_exit(127);
        open STDOUT, '>&', $slave or POSIX::_exit(127);
        open STDERR, '>&', $slave or POSIX::_exit(127);
        local %ENV = ( %ENV, $env ? %$env : () );
        local $ENV{TERM} = 'xterm' unless defined $ENV{TERM};
        exec { $argv->[0] } @$argv or POSIX::_exit(127);
    }
    $pty->close_slave;
    return bless {
        pid => $pid,
        pty => $pty,
        buf => q{},
        sel => IO::Select->new($pty),
    }, $class;
}

sub pid { my ($self) = @_; return $self->{pid} }
sub pty { my ($self) = @_; return $self->{pty} }
sub buf { my ($self) = @_; return $self->{buf} }

# Drain output for up to $sec seconds.  Returns false at EOF so callers can
# distinguish an exited child from a quiet one.
sub pump {
    my ( $self, $sec ) = @_;
    my $deadline = time + $sec;
    while ( time < $deadline ) {
        next unless $self->{sel}->can_read(0.1);
        my $n = sysread( $self->{pty}, my $data, 4096 );
        return 0 if !defined $n || $n <= 0;
        $self->{buf} .= $data;
    }
    return 1;
}

# Write all bytes to the PTY master, retrying short writes.  Returns false on
# EOF or write error.
sub write_data {
    my ( $self, $data ) = @_;
    my $off = 0;
    my $len = length $data;
    while ( $off < $len ) {
        my $n = syswrite( $self->{pty}, $data, $len - $off, $off );
        return 0 if !defined $n || $n <= 0;
        $off += $n;
    }
    return 1;
}

# Close the PTY and make a non-blocking attempt to reap the child.  The wait
# path normally reaps it first; this is a safety net for early failures.
sub close_session {
    my ($self) = @_;
    close( $self->{pty} ) if $self->{pty};
    waitpid( $self->{pid}, POSIX::WNOHANG() );
    return;
}

# Wait for the child to exit, draining the pty as we go so a child
# writing its terminal-restoration output can never block on a full tty
# buffer.  The child counts as gone when it is reaped or when the pty
# reaches end-of-file (the slave side is closed on process exit).  The
# EOF test matters because pump() returns immediately at EOF: polling
# only waitpid() with no minimum delay can exhaust every retry before a
# just-exited child becomes reapable and wrongly report a timeout.
sub wait_exit {
    my ( $self, $tries ) = @_;

    $tries //= 25;
    for ( 1 .. $tries ) {
        my $reaped = waitpid( $self->{pid}, POSIX::WNOHANG() );
        return 1 if $reaped != 0;
        unless ( $self->pump(0.1) ) {

            # EOF: the child closed the slave and is exiting.  Reap it
            # if it is already reapable so the caller can inspect $?.
            waitpid( $self->{pid}, POSIX::WNOHANG() );
            return 1;
        }
    }
    return 0;
}

# Decode the tiny escape language accepted on the command line: \xHH plus the
# common C escapes.  Unknown escapes are copied literally.
sub main::decode_input {
    my ($s) = @_;
    my %esc = ( n => 10, r => 13, t => 9, e => 27, q{\\} => 92 );
    my $out = q{};
    my $i   = 0;
    my $len = length $s;
    while ( $i < $len ) {
        if ( substr( $s, $i, 1 ) eq q{\\} && $i + 1 < $len ) {
            my $nxt = substr( $s, $i + 1, 1 );
            if ( $nxt eq 'x' && $i + 3 < $len ) {
                $out .= chr( hex( substr( $s, $i + 2, 2 ) ) );
                $i += 4;
                next;
            }
            if ( exists $esc{$nxt} ) {
                $out .= chr( $esc{$nxt} );
                $i += 2;
                next;
            }
        }
        $out .= substr( $s, $i, 1 );
        $i++;
    }
    return $out;
}

# Run one program under the PTY helper.  The optional timing tuple controls
# startup, inter-sequence delay and final drain; each input sequence is decoded
# and written in order.
sub main::run {
    my ( $argv, $inputs, $env, @timing ) = @_;
    my ( $startup, $delay, $drain ) = @timing;
    $inputs  = []  unless defined $inputs;
    $startup = 2.0 unless defined $startup;
    $delay   = 1.0 unless defined $delay;
    $drain   = 2.0 unless defined $drain;
    my $session = Session->new( $argv, $env );
    $session->pump($startup);

    for my $seq (@$inputs) {
        last unless $session->write_data( main::decode_input($seq) );
        $session->pump($delay);
    }
    $session->pump($drain);
    my $out = $session->buf;
    $session->close_session;
    return $out;
}

# Command-line wrapper: split argv at "--", run the program, and print its
# decoded UTF-8 transcript.  The module is silent when loaded by a test.
sub main::cli {
    my @args = @ARGV;
    my $sep  = scalar @args;
    for my $i ( 0 .. $#args ) {
        if ( $args[$i] eq q{--} ) {
            $sep = $i;
            last;
        }
    }
    my @argv   = @args[ 0 .. $sep - 1 ];
    my @inputs = @args[ $sep + 1 .. $#args ];
    my $out    = main::run( \@argv, \@inputs );
    binmode STDOUT, ':encoding(UTF-8)';
    print Encode::decode( 'UTF-8', $out );
    return;
}

main::cli() unless caller;

1;
