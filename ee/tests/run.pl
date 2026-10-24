#!/usr/bin/env perl

# Behavioural tests for ee(1).  Interactive behaviour is exercised
# through a pseudo-terminal; editing, menu navigation, saving, UTF-8
# input and terminal restoration are checked.
#
# Requires Perl 5 with the IO::Pty module; on OpenBSD install it with
# "pkg_add p5-IO-Tty" (devel/p5-IO-TTY).

use strict;
use warnings;

use Carp           ();
use Cwd            qw(abs_path);
use File::Basename qw(dirname);
use File::Temp     qw(tempdir);
use POSIX          ();

my $HERE = dirname( abs_path(__FILE__) );
my $ROOT = dirname( dirname($HERE) );
my $EE   = $ENV{EE} // "$ROOT/ee/ee";

unshift @INC, $HERE;
require pty_run;

my $PASS = 0;
my $FAIL = 0;
my @FAILED;

sub ok {
    my ($desc) = @_;
    $PASS++;
    print "ok $PASS - $desc\n";
    return;
}

sub notok {
    my ($desc) = @_;
    $FAIL++;
    push @FAILED, $desc;
    print "not ok - $desc\n";
    return;
}

sub assert_eq {
    my ( $desc, $expected, $actual ) = @_;
    if ( $expected eq $actual ) {
        ok($desc);
    }
    else {
        notok($desc);
        print "# expected: $expected\n";
        print "# actual:   $actual\n";
    }
    return;
}

# Return the file content with trailing newlines stripped, as a
# shell command substitution would.
sub read_text {
    my ($path) = @_;
    open my $fh, '<', $path or return q{};
    local $/ = undef;
    my $data = <$fh>;
    close $fh;
    $data = q{} unless defined $data;
    $data =~ s/\n+\z//x;
    return $data;
}

# Run a command with standard input connected to a pipe (not a
# terminal) and return its exit status, like a subprocess with
# stdin=PIPE, stdout inherited and stderr discarded.
sub run_no_tty {
    my (@cmd) = @_;
    pipe( my $rd, my $wr ) or Carp::croak "pipe: $!";
    my $pid = fork;
    Carp::croak "fork: $!" unless defined $pid;
    if ( $pid == 0 ) {
        open STDIN,  '<&', $rd         or POSIX::_exit(127);
        open STDERR, '>',  '/dev/null' or POSIX::_exit(127);
        exec { $cmd[0] } @cmd or POSIX::_exit(127);
    }
    close $rd;
    print {$wr} 'x';
    close $wr;
    waitpid( $pid, 0 );
    return $? >> 8;
}

# Capture `locale -a`, discarding diagnostics like the old qx did.
sub read_locale_list {
    my $list = q{};
    open my $loc, q{-|}, 'locale -a 2>/dev/null' or return $list;
    local $/ = undef;
    my $data = <$loc>;
    close $loc;
    return defined $data ? $data : $list;
}

# Locale policy: no locale configured means ee picks a UTF-8 one and the
# interface is English; an explicitly configured non-UTF-8 locale is
# rejected cleanly; an explicit UTF-8 locale works.
sub test_locale_policy {
    my ( $ee, $work ) = @_;

    my $no_locale = run(
        [ $ee, q{-?} ],
        [],
        {
            LC_ALL      => q{},
            LC_CTYPE    => q{},
            LANG        => q{},
            LC_MESSAGES => q{},
            TERM        => 'xterm'
        }
    );
    if (   index( $no_locale, 'usage:' ) >= 0
        && index( $no_locale, 'turn off info window' ) >= 0 )
    {
        ok('usage is English with no locale configured');
    }
    else {
        notok('usage is English with no locale configured');
    }

    my $c_locale = run( [ $ee, q{-?} ],
        [],
        { LC_ALL => 'C', LANG => 'C', LC_MESSAGES => 'C', TERM => 'xterm' } );
    if (   index( $c_locale, 'usage:' ) < 0
        && index( $c_locale, 'UTF-8 locale' ) >= 0 )
    {
        ok('non-UTF-8 locale C is rejected cleanly');
    }
    else {
        notok('non-UTF-8 locale C is rejected cleanly');
    }

    # Choose an explicit UTF-8 locale that this host actually provides.
    my $locale_list = read_locale_list();
    my $utf8_locale;
    for my $cand ( 'C.UTF-8', 'C.utf8', 'en_US.UTF-8', 'en_US.utf8' ) {
        if ( $locale_list =~ /^\Q$cand\E$/mix ) {
            $utf8_locale = $cand;
            last;
        }
    }

    return unless defined $utf8_locale;

    my $utf8_out = run(
        [ $ee, q{-?} ],
        [],
        {
            LC_ALL      => $utf8_locale,
            LANG        => $utf8_locale,
            LC_MESSAGES => $utf8_locale,
            TERM        => 'xterm'
        }
    );
    if ( index( $utf8_out, 'usage:' ) >= 0 ) {
        ok("usage is English under $utf8_locale");
    }
    else {
        notok("usage is English under $utf8_locale");
    }

    my $path = "$work/locfile.txt";
    run(
        [ $ee,    '-i',   $path ],
        [ "café", "\x1b", 'a', 'a' ],
        {
            LC_ALL => $utf8_locale,
            LANG   => $utf8_locale,
            TERM   => 'xterm'
        }
    );
    assert_eq( "UTF-8 edit under $utf8_locale", 'café', read_text($path) );
    return;
}

sub main {
    unless ( -x $EE ) {
        notok('ee binary exists');
        print "\n";
        print "pass: $PASS  fail: $FAIL\n";
        return 1;
    }
    ok('ee binary exists');

    my $tmpdir = $ENV{TMPDIR} // '/tmp';
    my $home =
      tempdir( 'ee-run-home.XXXXXX', DIR => $tmpdir, CLEANUP => 1 );
    local $ENV{HOME} = $home;
    my $work = tempdir( 'ee-test.XXXXXX', DIR => $tmpdir, CLEANUP => 1 );

    # refuses to run without a terminal
    my $rc = run_no_tty( $EE, q{-?} );
    assert_eq( 'requires a terminal', 1, $rc );

    # usage output through a pty
    my $usage = run( [ $EE, q{-?} ], [], { TERM => 'xterm' } );
    if ( index( $usage, 'usage:' ) >= 0 && index( $usage, '-i' ) >= 0 ) {
        ok('usage shows options');
    }
    else {
        notok('usage shows options');
    }

    # edit, save and exit via menu, with UTF-8 input
    my $path = "$work/testfile.txt";
    run(
        [ $EE, '-i', $path ],
        [ "hello world\nsecond line café", "\x1b", 'a', 'a' ],
        { TERM => 'xterm' }
    );
    assert_eq(
        'edited file saved',
        "hello world\nsecond line café",
        read_text($path)
    );

    # the menu hides the cursor while it is open, so the cursor
    # block does not leave the first character of the highlighted
    # entry looking unselected, and restores it on close
    $path = "$work/menufile.txt";
    my $out =
      run( [ $EE, '-i', $path ], [ "\x1b", "\x1b" ], { TERM => 'xterm' } );
    if (   index( $out, "\x1b[?25l" ) >= 0
        && index( $out, "\x1b[?25h" ) >= 0 )
    {
        ok('menu hides and restores the cursor');
    }
    else {
        notok('menu hides and restores the cursor');
    }
    if ( index( $out, "\x1b[0;7ma) leave editor" ) >= 0 ) {
        ok('menu highlights the selected entry');
    }
    else {
        notok('menu highlights the selected entry');
    }

    # mode preserved for existing files
    open my $fh, '>', $path or Carp::croak "open $path: $!";
    print {$fh} "original\n";
    close $fh;
    chmod 0640, $path;
    run( [ $EE, '-i', $path ], [ 'xx', "\x1b", 'a', 'a' ],
        { TERM => 'xterm' } );
    assert_eq( 'content updated', 'xxoriginal', read_text($path) );
    assert_eq( 'mode preserved',
        '640', sprintf( '%o', ( stat $path )[2] & oct(777) ) );

    # new file created with normal umask
    $path = "$work/newfile.txt";
    run(
        [ $EE,    '-i',   $path ],
        [ 'data', "\x1b", 'a', 'a' ],
        { TERM => 'xterm' }
    );
    assert_eq( 'new file content', 'data', read_text($path) );

    # long lines (longer than the internal 512-byte read chunks)
    # survive a load/save round trip
    open my $long, '>', $path or Carp::croak "open $path: $!";
    my $content = ( 'x' x 3000 ) . "\n" . "short\n";
    print {$long} $content;
    close $long;
    run( [ $EE, '-i', $path ], [ "\x1b", 'a', 'a' ], { TERM => 'xterm' } );
    open my $in, '<', $path or Carp::croak "open $path: $!";
    my $first = <$in> // q{};
    close $in;
    $first =~ s/\n\z//x;
    assert_eq( 'long line preserved', 'x' x 3000, $first );

    # file without a final newline
    open my $noeol, '>', $path or Carp::croak "open $path: $!";
    print {$noeol} 'no trailing newline';
    close $noeol;
    run( [ $EE, '-i', $path ], [ "\x1b", 'a', 'a' ], { TERM => 'xterm' } );
    assert_eq(
        'file without final newline kept',
        'no trailing newline',
        read_text($path)
    );

    # "no save" path leaves the file untouched
    open my $keep, '>', $path or Carp::croak "open $path: $!";
    print {$keep} "keep\n";
    close $keep;
    run(
        [ $EE,    '-i',   $path ],
        [ 'junk', "\x1b", 'a', 'b' ],
        { TERM => 'xterm' }
    );
    assert_eq( 'no-save leaves file untouched', 'keep', read_text($path) );

    # ^S saves the buffer and ^Q quits an unmodified buffer
    $path = "$work/ctrlkeys.txt";
    run( [ $EE, '-i', $path ], [ "data", "\x13", "\x11" ],
        { TERM => 'xterm' } );
    assert_eq( '^S saves and ^Q quits', 'data', read_text($path) );

    # Esc cancels the save-as prompt instead of accepting an empty name
    my $cancel_out = run(
        [ $EE,    '-i' ],
        [ "data", "\x13", "\x1b", "\x11", 'b' ],
        { TERM => 'xterm' }
    );
    if ( index( $cancel_out, 'File name:' ) >= 0 ) {
        ok('save-as prompt is shown');
    }
    else {
        notok('save-as prompt is shown');
    }

    test_locale_policy( $EE, $work );

    print "\n";
    print "pass: $PASS  fail: $FAIL\n";
    if ( $FAIL > 0 ) {
        my $names = join q{}, map { " $_" } @FAILED;
        print "failed tests:$names\n";
        return 1;
    }
    return 0;
}

exit main();
