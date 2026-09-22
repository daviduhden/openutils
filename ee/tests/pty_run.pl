#!/usr/bin/env perl

# pty_run.pl - run a program on a pseudo-terminal, feed it key
# sequences, and return (or print) its output.
#
# Requires Perl 5 with the IO::Pty module; on OpenBSD install it with
# "pkg_add p5-IO-Tty" (devel/p5-IO-TTY).  This helper is used by the
# ee behavioural tests only; the other OpenUtils test suites are
# POSIX shell.
#
# usage: pty_run.pl <program> <args...> -- <input-sequence>...
#
# Each input sequence may contain \xHH escapes (or plain text) and is
# sent to the program with a small delay between sequences, ending
# with a final drain period.

use strict;
use warnings;

use Encode ();
use IO::Pty;
use IO::Select;
use POSIX       ();
use Time::HiRes qw(time);

$SIG{PIPE} = 'IGNORE';

package Session;

sub new {
    my ( $class, $argv, $env ) = @_;
    my $pty = IO::Pty->new or die "cannot allocate a pty: $!";
    my $pid = fork;
    die "cannot fork: $!" unless defined $pid;
    if ( $pid == 0 ) {
        my $slave = $pty->slave;
        $pty->make_slave_controlling_terminal;
        $pty->close;
        open STDIN,  '<&', $slave or POSIX::_exit(127);
        open STDOUT, '>&', $slave or POSIX::_exit(127);
        open STDERR, '>&', $slave or POSIX::_exit(127);
        if ($env) {
            $ENV{$_} = $env->{$_} for keys %$env;
        }
        $ENV{TERM} = 'xterm' unless defined $ENV{TERM};
        no warnings 'exec';
        exec { $argv->[0] } @$argv;
        POSIX::_exit(127);
    }
    $pty->close_slave;
    return bless {
        pid => $pid,
        pty => $pty,
        buf => '',
        sel => IO::Select->new($pty),
    }, $class;
}

sub pid { return $_[0]->{pid} }
sub pty { return $_[0]->{pty} }
sub buf { return $_[0]->{buf} }

sub pump {
    my ( $self, $sec ) = @_;
    my $deadline = time + $sec;
    while ( time < $deadline ) {
        next unless $self->{sel}->can_read(0.1);
        my $n = sysread( $self->{pty}, my $data, 4096 );
        return 0 unless defined $n && $n > 0;
        $self->{buf} .= $data;
    }
    return 1;
}

sub write {
    my ( $self, $data ) = @_;
    my $off = 0;
    my $len = length $data;
    while ( $off < $len ) {
        my $n = syswrite( $self->{pty}, $data, $len - $off, $off );
        return 0 unless defined $n && $n > 0;
        $off += $n;
    }
    return 1;
}

sub close {
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

package main;

sub decode_input {
    my ($s) = @_;
    my %esc = ( n => 10, r => 13, t => 9, e => 27, '\\' => 92 );
    my $out = '';
    my $i   = 0;
    my $len = length $s;
    while ( $i < $len ) {
        if ( substr( $s, $i, 1 ) eq '\\' && $i + 1 < $len ) {
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

sub run {
    my ( $argv, $inputs, $env, $startup, $delay, $drain ) = @_;
    $inputs  = []  unless defined $inputs;
    $startup = 2.0 unless defined $startup;
    $delay   = 1.0 unless defined $delay;
    $drain   = 2.0 unless defined $drain;
    my $session = Session->new( $argv, $env );
    $session->pump($startup);
    for my $seq (@$inputs) {
        last unless $session->write( decode_input($seq) );
        $session->pump($delay);
    }
    $session->pump($drain);
    my $out = $session->buf;
    $session->close;
    return $out;
}

sub cli {
    my @args = @ARGV;
    my $sep  = scalar @args;
    for my $i ( 0 .. $#args ) {
        if ( $args[$i] eq '--' ) {
            $sep = $i;
            last;
        }
    }
    my @argv   = @args[ 0 .. $sep - 1 ];
    my @inputs = @args[ $sep + 1 .. $#args ];
    my $out    = run( \@argv, \@inputs );
    binmode STDOUT, ':encoding(UTF-8)';
    print Encode::decode( 'UTF-8', $out );
    return;
}

cli() unless caller;
