#!/usr/bin/env perl

# PTY-based regression tests for ee: signal handling and terminal
# robustness.  Generous per-key delays are used because menu redraws
# are timing-sensitive.
#
# Terminal handling is delegated to ncursesw, so the historical tests
# for the bundled terminfo parser are kept only as "must not crash on a
# hostile TERM" checks; the final check exercises an unknown terminal
# name, which ncursesw must reject without crashing.
#
# Requires Perl 5 with the IO::Pty module; on OpenBSD install it with
# "pkg_add p5-IO-Tty" (devel/p5-IO-TTY).

use strict;
use warnings;

use Carp           ();
use Cwd            qw(abs_path);
use File::Basename qw(dirname);
use File::Path     qw(make_path);
use File::Temp     qw(tempdir);
use POSIX          qw(WNOHANG);

my $HERE = dirname( abs_path(__FILE__) );
my $ROOT = dirname( dirname($HERE) );
my $EE   = $ENV{EE} // "$ROOT/ee/ee";

unshift @INC, $HERE;
require pty_run;

# TIOCSWINSZ is 0x5414 on Linux and _IOW('t', 103, struct winsize)
# on the BSDs (including OpenBSD).
my $TIOCSWINSZ = $^O eq 'linux' ? 0x5414 : 0x80087467;

my $PASS = 0;
my $FAIL = 0;
my @FAILED;

# Record a passing assertion and its label.
sub ok {
    my ($desc) = @_;
    $PASS++;
    print "ok $PASS - $desc\n";
    return;
}

# Record a failing assertion without aborting the remaining checks.
sub notok {
    my ($desc) = @_;
    $FAIL++;
    push @FAILED, $desc;
    print "not ok - $desc\n";
    return;
}

# Check a boolean condition and emit a diagnostic on failure.
sub check {
    my ( $desc, $value ) = @_;
    if ($value) {
        ok($desc);
    }
    else {
        notok($desc);
    }
    return;
}

# Write a minimal compiled terminfo entry (magic 282).
# Generate a hostile terminfo entry for the unknown-terminal checks.
sub build_terminfo {
    my ( $path, $clear, $names ) = @_;
    my $NS      = 190;
    my @numbers = (0xFFFF) x 40;
    $numbers[0] = 80;
    $numbers[2] = 24;
    my @soff   = (0xFFFF) x $NS;
    my $table  = q{};
    my $addstr = sub {
        my ( $idx, $s ) = @_;
        $soff[$idx] = length $table;
        $table .= $s . "\0";
        return;
    };
    $addstr->( 5,   $clear );
    $addstr->( 10,  "\033[%i%d;%dH" );
    $addstr->( 104, q{} );
    my $hdr = pack( 'v6',
        282, length($names), 0, scalar(@numbers), $NS, length($table) );
    my $pad = ( length($names) % 2 ) ? "\0" : q{};
    open my $fh, '>', $path or Carp::croak "open $path: $!";
    binmode $fh;
    print {$fh} $hdr, $names, $pad;
    print {$fh} pack( 'v*', @numbers );
    print {$fh} pack( 'v*', @soff );
    print {$fh} $table;
    close $fh;
    return;
}

# Send SIGINT during editing and verify that the terminal is restored.
sub sigint_test {
    my ($work) = @_;
    my $session =
      Session->new( [ $EE, '-i', "$work/sigint.txt" ], { TERM => 'xterm' } );
    $session->pump(2.5);
    $session->write_data('unsaved');
    $session->pump(1);
    kill 'INT', $session->pid;
    $session->pump(2.5);
    my $exited   = $session->wait_exit;
    my $restored = index( $session->buf, "\x1b[?1l\x1b>" ) >= 0 ? 1 : 0;
    my $saved    = -e "$work/sigint.txt"                        ? 1 : 0;
    $session->close_session;
    return ( $exited, $restored, $saved );
}

# Send SIGWINCH and verify that the display survives a resize.
sub sigwinch_test {
    my ($work) = @_;
    my $session =
      Session->new( [ $EE, '-i', "$work/resize.txt" ], { TERM => 'xterm' } );
    $session->pump(2.5);
    $session->write_data('kept text');
    $session->pump(1);
    for my $size ( [ 40, 100 ], [ 10, 30 ], [ 30, 60 ] ) {
        my ( $rows, $cols ) = @$size;

        # struct winsize is four unsigned shorts ('H' would pack a hex
        # string and set a bogus size).
        my $ws = pack( 'SSSS', $rows, $cols, 0, 0 );
        ioctl( $session->pty, $TIOCSWINSZ, $ws );
        kill 'WINCH', $session->pid;
        $session->pump(1.5);
    }
    $session->write_data("\x1b");
    $session->pump(1);
    $session->write_data('a');
    $session->pump(1);
    $session->write_data('a');
    $session->pump(2);
    my $data = q{};

    if ( -e "$work/resize.txt" ) {
        open my $fh, '<', "$work/resize.txt" or Carp::croak "open: $!";
        binmode $fh;
        local $/ = undef;
        $data = <$fh> // q{};
        close $fh;
    }
    $session->close_session;
    return index( $data, 'kept text' ) >= 0 ? 1 : 0;
}

# Run ee against a synthetic entry with a hostile clear string.
# Run ee against a deliberately malformed terminfo entry and require a clean
# exit rather than a crash.
sub tinfo_test {
    my ( $work, $clear ) = @_;
    my $tdir = "$work/terminfo";
    make_path("$tdir/o");
    build_terminfo( "$tdir/o/openutils-test", $clear,
        "openutils-test|t|malformed terminfo test\0" );
    my $session = Session->new( [ $EE, '-i', "$work/tinf.txt" ],
        { TERM => 'openutils-test', TERMINFO => $tdir } );
    $session->pump(2.5);
    $session->write_data('x');
    $session->pump(1.5);
    my $wpid   = waitpid( $session->pid, WNOHANG );
    my $status = $?;
    my $sig    = $status & 0x7f;
    my $crashed =
      ( $wpid != 0 && $status != 0 && $sig != 0 && $sig != 0x7f )
      ? 1
      : 0;
    $session->close_session;
    return $crashed;
}

# An unknown terminal name must be rejected by ncursesw with a clean,
# non-zero exit, never a crash.
# Run ee with an unknown TERM value and require a clean rejection.
sub noterm_test {
    my ($work) = @_;
    my $session = Session->new(
        [ $EE, '-i', "$work/noterm.txt" ],
        { TERM => 'openutils-no-such-terminal' }
    );
    $session->pump(2.5);
    my $exited = $session->wait_exit;
    my $status = $?;
    my $sig    = $status & 0x7f;
    my $crashed =
      ( $exited && $status != 0 && $sig != 0 && $sig != 0x7f )
      ? 1
      : 0;
    $session->close_session;
    return ( $exited, $crashed );
}

# Run the signal, terminfo and unknown-terminal cases and report failures.
sub main {
    unless ( -x $EE ) {
        notok('ee binary exists');
        print "\n";
        print "pass: $PASS  fail: $FAIL\n";
        return 1;
    }
    ok('ee binary exists');

    my $tmpdir = $ENV{TMPDIR} // '/tmp';
    my $home   = tempdir( 'ee-sig-home.XXXXXX', DIR => $tmpdir, CLEANUP => 1 );
    local $ENV{HOME} = $home;
    my $work = tempdir( 'ee-sig-test.XXXXXX', DIR => $tmpdir, CLEANUP => 1 );
    my ( $exited, $restored, $saved ) = sigint_test($work);
    check( 'SIGINT exits the editor',      $exited );
    check( 'SIGINT restores the terminal', $restored );
    check( 'SIGINT does not save',         !$saved );

    check( 'SIGWINCH keeps the editor working', sigwinch_test($work) );

    for my $bad ( '%{0}%{0}/%d', '%{0}%{0}%%d', '%{123', '%p9', '%P9',
        '%g9', '%{99999999999999999999}', '$<12x' )
    {
        check( "terminfo '$bad' does not crash", !tinfo_test( $work, $bad ) );
    }

    my ( $noterm_exited, $noterm_crashed ) = noterm_test($work);
    check( 'unknown TERM exits cleanly',  $noterm_exited );
    check( 'unknown TERM does not crash', !$noterm_crashed );

    print "pass: $PASS  fail: $FAIL\n";
    if ( $FAIL > 0 ) {
        my $names = join q{}, map { " $_" } @FAILED;
        print "failed tests:$names\n";
        return 1;
    }
    return 0;
}

exit main();
