#!/usr/bin/env perl

# Second-phase regression tests for ee: resize in every modal state,
# safe exit, strict UTF-8/NUL policy, Unicode insertion bounds, display
# of wide and combining characters, tiny terminals, help paging and
# shell round trips.
#
# Requires Perl 5 with the IO::Pty module; on OpenBSD install it with
# "pkg_add p5-IO-Tty" (devel/p5-IO-TTY).

use strict;
use warnings;

use Cwd            qw(abs_path);
use File::Basename qw(dirname);
use File::Temp     qw(tempdir);
use POSIX          qw(WNOHANG);
use Time::HiRes    qw(sleep);

my $HERE = dirname( abs_path(__FILE__) );
my $ROOT = dirname( dirname($HERE) );
my $EE   = $ENV{EE} // "$ROOT/ee/ee";

require "$HERE/pty_run.pl";

my $TIOCSWINSZ = $^O eq 'linux' ? 0x5414 : 0x80087467;

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

sub check {
    my ( $desc, $value ) = @_;
    $value ? ok($desc) : notok($desc);
    return;
}

sub write_raw {
    my ( $path, $bytes ) = @_;
    open my $fh, '>:raw', $path or die "open $path: $!";
    print {$fh} $bytes;
    close $fh;
    return;
}

sub read_raw {
    my ($path) = @_;
    return '' unless -e $path;
    open my $fh, '<:raw', $path or die "open $path: $!";
    local $/;
    my $data = <$fh>;
    close $fh;
    return defined $data ? $data : '';
}

sub resize {
    my ( $s, $rows, $cols ) = @_;
    my $ws = pack( 'HHHH', $rows, $cols, 0, 0 );
    ioctl( $s->pty, $TIOCSWINSZ, $ws );
    kill 'WINCH', $s->pid;
    $s->pump(1.2);
    return;
}

sub wait_exit {
    my ( $s, $tries ) = @_;
    $tries //= 25;
    for ( 1 .. $tries ) {
        my $w = waitpid( $s->pid, WNOHANG );
        return 1 if $w != 0;
        sleep 0.1;
    }
    return 0;
}

# Open a file, type text, save with ^S and quit with ^Q.
sub simple_edit {
    my ( $path, $text, $extra_inputs ) = @_;
    my @inputs = ( $text, "\x13", "\x11" );
    if ($extra_inputs) {
        @inputs = ( $text, @$extra_inputs );
    }
    run( [ $EE, '-i', $path ], \@inputs, { TERM => 'xterm' } );
    return;
}

sub test_resize_in_menu {
    my ($work) = @_;
    my $path = "$work/menu-resize.txt";
    my $s = Session->new( [ $EE, '-i', $path ], { TERM => 'xterm' } );
    $s->pump(1.5);
    $s->write("kept");
    $s->pump(0.5);
    $s->write("\x1b");    # open main menu
    $s->pump(0.8);
    resize( $s, 8, 30 );
    $s->write("\x1b");    # cancel the menu
    $s->pump(0.8);
    $s->write("\x13");    # save
    $s->pump(1.0);
    $s->write("\x11");    # quit
    my $exited = wait_exit($s);
    $s->close;
    return ( $exited && read_raw($path) eq "kept\n" ) ? 1 : 0;
}

sub test_resize_in_prompt {
    my ($work) = @_;
    my $path = "$work/prompt-resize.txt";
    write_raw( $path, "alpha\n" );
    my $s = Session->new( [ $EE, '-i', $path ], { TERM => 'xterm' } );
    $s->pump(1.5);
    $s->write("\x05");    # ^E search prompt
    $s->pump(0.6);
    $s->write("alp");
    $s->pump(0.4);
    resize( $s, 40, 100 );
    $s->write("\x1b");    # cancel the search prompt
    $s->pump(0.6);
    $s->write("\x11");    # quit (unmodified)
    my $exited = wait_exit($s);
    $s->close;
    return $exited;
}

sub test_resize_in_confirm {
    my ($work) = @_;
    my $a = "$work/confirm-a.txt";
    my $b = "$work/confirm-b.txt";
    write_raw( $a, "AAA\n" );
    write_raw( $b, "OLD\n" );
    my $s = Session->new( [ $EE, '-i', $a ], { TERM => 'xterm' } );
    $s->pump(1.5);
    $s->write("\x03");          # ^C command prompt
    $s->pump(0.5);
    $s->write("write $b\n");    # asks to overwrite
    $s->pump(0.8);
    resize( $s, 30, 60 );
    $s->write("n");             # do not overwrite
    $s->pump(0.6);
    $s->write("\x11");          # quit (unmodified; we did not modify)
    my $exited = wait_exit($s);
    $s->close;
    return ( $exited && read_raw($b) eq "OLD\n" ) ? 1 : 0;
}

sub test_resize_in_help {
    my ($work) = @_;
    my $path = "$work/help-resize.txt";
    my $s = Session->new( [ $EE, '-i', $path ], { TERM => 'xterm' } );
    $s->pump(1.5);
    $s->write("\x1b");    # menu
    $s->pump(0.6);
    $s->write("b");       # help
    $s->pump(0.8);
    resize( $s, 12, 40 );
    $s->write(" ");       # next page
    $s->pump(0.5);
    resize( $s, 40, 100 );
    $s->write("\x1b");    # close help
    $s->pump(0.5);
    $s->write("\x11");    # quit
    my $exited = wait_exit($s);
    $s->close;
    return $exited;
}

sub test_quit_cancel_discard_save {
    my ($work) = @_;
    my $path = "$work/quit.txt";
    write_raw( $path, "orig\n" );

    # cancel the leave menu, buffer must stay modified
    write_raw( $path, "orig\n" );
    run(
        [ $EE, '-i', $path ],
        [ 'X', "\x11", "\x1b", "\x11", 'b' ],
        { TERM => 'xterm' }
    );
    my $discarded = read_raw($path) eq "orig\n" ? 1 : 0;

    # save through the leave menu
    write_raw( $path, "orig\n" );
    run(
        [ $EE, '-i', $path ],
        [ 'Y', "\x11", 'a' ],
        { TERM => 'xterm' }
    );
    my $saved = read_raw($path) eq "Yorig\n" ? 1 : 0;

    return ( $discarded && $saved ) ? 1 : 0;
}

sub test_overwrite_cancel {
    my ($work) = @_;
    my $a = "$work/ow-a.txt";
    my $b = "$work/ow-b.txt";
    write_raw( $a, "AAA\n" );
    write_raw( $b, "KEEP\n" );
    run(
        [ $EE, '-i', $a ],
        [ "\x03", "write $b\n", "\x1b", "\x11" ],
        { TERM => 'xterm' }
    );
    return read_raw($b) eq "KEEP\n" ? 1 : 0;
}

sub test_wide_and_combining {
    my ($work) = @_;
    my $path = "$work/wide.txt";

    # U+4E16 (wide CJK) then 'e' + U+0301 (combining acute)
    my $wide = "\xe4\xb8\x96";
    my $comb = "e\xcc\x81";
    run(
        [ $EE, '-i', $path ],
        [ $wide . $comb, "\x13", "\x11" ],
        { TERM => 'xterm' }
    );
    return read_raw($path) eq ( $wide . $comb . "\n" ) ? 1 : 0;
}

sub test_backspace_utf8 {
    my ($work) = @_;
    my $path  = "$work/backspace.txt";
    my $wide  = "\xe4\xb8\x96";    # U+4E16, 3 bytes
    my $input = $wide . "\x7f";    # then delete it
    run( [ $EE, '-i', $path ], [ $input, "Z", "\x13", "\x11" ],
        { TERM => 'xterm' } );
    return read_raw($path) eq "Z\n" ? 1 : 0;
}

sub test_invalid_utf8_rejected {
    my ($work) = @_;
    my $path = "$work/invalid.txt";
    write_raw( $path, "abc\xffdef\n" );
    my $out = run( [ $EE, '-i', $path ], [], { TERM => 'xterm' } );
    return ( index( $out, 'not valid UTF-8' ) >= 0 ) ? 1 : 0;
}

sub test_nul_rejected {
    my ($work) = @_;
    my $path = "$work/nul.txt";
    write_raw( $path, "abc\x00def\n" );
    my $out = run( [ $EE, '-i', $path ], [], { TERM => 'xterm' } );
    return ( index( $out, 'not a text file' ) >= 0 ) ? 1 : 0;
}

sub test_code_point_bounds {
    my ($work) = @_;
    my $path = "$work/codes.txt";

    # U+10FFFF is valid; U+110000 and U+D800 are rejected.
    run(
        [ $EE, '-i', $path ],
        [ "\x01", "1114111\n", "\x01", "1114112\n", "\x01", "55296\n",
            "\x13", "\x11" ],
        { TERM => 'xterm' }
    );
    return read_raw($path) eq "\xf4\x8f\xbf\xbf\n" ? 1 : 0;
}

sub test_tiny_then_grow {
    my ($work) = @_;
    my $path = "$work/tiny.txt";
    my $s = Session->new( [ $EE, '-i', $path ], { TERM => 'xterm' } );
    $s->pump(1.5);
    resize( $s, 3, 10 );
    $s->write("hi");
    $s->pump(0.5);
    resize( $s, 40, 100 );
    $s->write("there");
    $s->pump(0.5);
    $s->write("\x13");
    $s->pump(1.0);
    $s->write("\x11");
    my $exited = wait_exit($s);
    $s->close;
    return ( $exited && read_raw($path) eq "hithere\n" ) ? 1 : 0;
}

sub test_control_characters {
    my ($work) = @_;
    my $path = "$work/control.txt";
    my $s = Session->new( [ $EE, '-i', $path ], { TERM => 'xterm' } );
    $s->pump(1.5);

    # Insert C0 controls and DEL through the character-code command; the
    # editor must show them visibly (^[, ^G, ^M, ^?) and preserve the
    # bytes without ever emitting them as terminal control sequences.
    for my $code ( 27, 7, 13, 127 ) {
        $s->write("\x01");
        $s->pump(0.4);
        $s->write("$code\n");
        $s->pump(0.4);
    }
    $s->write("\x13");
    $s->pump(1.0);
    $s->write("\x11");
    my $exited = wait_exit($s);
    my $out = $s->buf;
    $s->close;

    my $bytes  = read_raw($path);
    my $expect = pack( 'C*', 27, 7, 13, 127 ) . "\n";
    my $visible = index( $out, '^[' ) >= 0 && index( $out, '^G' ) >= 0;
    return ( $exited && $bytes eq $expect && $visible ) ? 1 : 0;
}

sub test_resize_stress {
    my ($work) = @_;
    my $path = "$work/stress.txt";
    my $s = Session->new( [ $EE, '-i', $path ], { TERM => 'xterm' } );
    $s->pump(1.5);
    for my $size ( [ 3, 10 ], [ 24, 80 ], [ 5, 20 ], [ 40, 120 ],
        [ 2, 5 ], [ 30, 100 ] )
    {
        resize( $s, @$size );
        $s->write("x");
        $s->pump(0.3);
    }
    $s->write("\x13");
    $s->pump(1.0);
    $s->write("\x11");
    my $exited = wait_exit($s);
    $s->close;
    return ( $exited && read_raw($path) eq "xxxxxx\n" ) ? 1 : 0;
}

sub test_shell_roundtrip {
    my ($work) = @_;
    my $path = "$work/shell.txt";
    write_raw( $path, "before\n" );
    my $s = Session->new( [ $EE, '-i', $path ], { TERM => 'xterm' } );
    $s->pump(1.5);
    $s->write("\x0f");              # ^O: end of line
    $s->pump(0.4);
    $s->write(" after");
    $s->pump(0.5);
    $s->write("\x03");              # ^C
    $s->pump(0.5);
    $s->write("!true\n");           # run a trivial shell command
    $s->pump(1.2);
    $s->write("\n");                # press return to continue
    $s->pump(1.0);
    $s->write("\x13");              # save
    $s->pump(1.0);
    $s->write("\x11");              # quit
    my $exited = wait_exit($s);
    $s->close;
    return ( $exited && read_raw($path) eq "before after\n" ) ? 1 : 0;
}

sub main {
    unless ( -x $EE ) {
        notok('ee binary exists');
        print "\npass: $PASS  fail: $FAIL\n";
        return 1;
    }
    ok('ee binary exists');

    my $tmpdir = $ENV{TMPDIR} // '/tmp';
    my $work   = tempdir( 'ee-modern.XXXXXX', DIR => $tmpdir, CLEANUP => 1 );

    check( 'resize inside the menu',         test_resize_in_menu($work) );
    check( 'resize inside a prompt',         test_resize_in_prompt($work) );
    check( 'resize inside a confirmation',   test_resize_in_confirm($work) );
    check( 'resize inside help',             test_resize_in_help($work) );
    check( 'quit cancel / discard / save',   test_quit_cancel_discard_save($work) );
    check( 'overwrite cancelled with Esc',   test_overwrite_cancel($work) );
    check( 'wide and combining characters',  test_wide_and_combining($work) );
    check( 'backspace removes a multibyte character',
        test_backspace_utf8($work) );
    check( 'invalid UTF-8 is rejected',      test_invalid_utf8_rejected($work) );
    check( 'NUL byte is rejected',           test_nul_rejected($work) );
    check( 'character-code bounds',          test_code_point_bounds($work) );
    check( 'tiny terminal then grow',        test_tiny_then_grow($work) );
    check( 'control characters are preserved and visible',
        test_control_characters($work) );
    check( 'resize stress',                  test_resize_stress($work) );
    check( 'shell round trip',               test_shell_roundtrip($work) );

    print "\npass: $PASS  fail: $FAIL\n";
    if ( $FAIL > 0 ) {
        my $names = join '', map { " $_" } @FAILED;
        print "failed tests:$names\n";
        return 1;
    }
    return 0;
}

exit main();
