#!/usr/bin/env perl

# Regression tests for the key binding sets (vi), configuration
# persistence and the paragraph formatter's diff/code protection, the
# whole-document formatter and its paragraph boundaries.
#
# Requires Perl 5 with the IO::Pty module; on OpenBSD install it with
# "pkg_add p5-IO-Tty" (devel/p5-IO-TTY).

use strict;
use warnings;

use Cwd            qw(abs_path);
use File::Basename qw(dirname);
use File::Temp     qw(tempdir);
use POSIX          qw(WNOHANG);

my $HERE = dirname( abs_path(__FILE__) );
my $ROOT = dirname( dirname($HERE) );
my $EE   = $ENV{EE} // "$ROOT/ee/ee";

require "$HERE/pty_run.pl";

my $PASS = 0;
my $FAIL = 0;
my @FAILED;

sub ok {
    my ($desc) = @_;
    $PASS++;
    print "ok $PASS - $desc\n";
}

sub notok {
    my ($desc) = @_;
    $FAIL++;
    push @FAILED, $desc;
    print "not ok - $desc\n";
}

sub check {
    my ( $desc, $value ) = @_;
    $value ? ok($desc) : notok($desc);
}

sub write_raw {
    my ( $path, $bytes ) = @_;
    open my $fh, '>:raw', $path or die "open $path: $!";
    print {$fh} $bytes;
    close $fh;
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

# Run ee configured for vi bindings (through a private $HOME/.init.ee).
sub vi_session {
    my ( $work, $path ) = @_;
    write_raw( "$work/.init.ee", "VI\n" );
    return Session->new( [ $EE, '-i', $path ],
        { HOME => $work, TERM => 'xterm' } );
}

sub test_vi_append_and_wq {
    my ($work) = @_;
    my $path = "$work/vi-append.txt";
    write_raw( $path, "hello\n" );

    my $s = vi_session( $work, $path );
    $s->pump(1.5);
    $s->write("\x24");       # $ - end of line
    $s->pump(0.3);
    $s->write("a world");    # a - append, then insert text
    $s->pump(0.4);
    $s->write("\x1b");       # Esc - back to normal mode
    $s->pump(0.3);
    $s->write(":wq\r");      # write and quit
    $s->pump(1.2);
    my $exited = $s->wait_exit;
    $s->close;
    return ( $exited && read_raw($path) eq "hello world\n" ) ? 1 : 0;
}

sub test_vi_delete_line {
    my ($work) = @_;
    my $path = "$work/vi-dd.txt";
    write_raw( $path, "one\ntwo\nthree\n" );

    my $s = vi_session( $work, $path );
    $s->pump(1.5);
    $s->write("dd");    # delete the current line
    $s->pump(0.4);
    $s->write(":wq\r");
    $s->pump(1.2);
    my $exited = $s->wait_exit;
    $s->close;
    return ( $exited && read_raw($path) eq "two\nthree\n" ) ? 1 : 0;
}

sub test_config_roundtrip {
    my ($work) = @_;
    my $path = "$work/vi-config.txt";
    write_raw( $path, "kept\n" );

    my $s = vi_session( $work, $path );
    $s->pump(1.5);
    $s->write("\x1b");    # normal mode Esc opens the main menu
    $s->pump(0.6);
    $s->write("e");       # settings (item 5)
    $s->pump(0.6);

    # Leave the settings menu; the editor must still work and quit.
    $s->write("\x1b");
    $s->pump(0.4);
    $s->write(":q\r");
    $s->pump(0.8);
    my $exited = $s->wait_exit;
    $s->close;
    return $exited;
}

sub test_format_protects_diff {
    my ($work) = @_;
    my $path = "$work/patch.txt";
    my $patch =
        "Index: foo.c\n"
      . "===================================================================\n"
      . "--- foo.c\n"
      . "+++ foo.c\n"
      . "@@ -1,2 +1,3 @@\n"
      . " int main(void)\n"
      . "+{ return 0; }\n" . " \n";
    write_raw( $path, $patch );

    my $s = vi_session( $work, $path );
    $s->pump(1.5);
    $s->write("\x1b");    # open the main menu
    $s->pump(0.6);
    $s->write("g");       # miscellaneous (item 7)
    $s->pump(0.6);
    $s->write("a");       # format paragraph (item 1)
    $s->pump(0.8);        # the formatter must leave the diff alone
    $s->write(":q\r");    # leave without changes
    $s->pump(0.8);
    my $exited = $s->wait_exit;
    $s->close;
    return ( $exited && read_raw($path) eq $patch ) ? 1 : 0;
}

# Reflow every paragraph in one go.  Each paragraph must be wrapped to
# at most 72 columns on its own: blank lines survive and text from
# separate paragraphs is never joined.
sub test_format_whole_document {
    my ($work) = @_;
    my $path = "$work/whole.txt";
    my $first =
        "This first paragraph is deliberately written to be long enough "
      . "that the formatter has to wrap it onto several lines while it "
      . "reflows the whole document at once.\n";
    my $second = "A short second paragraph.\n";
    my $third =
        "A third paragraph that is also long enough to require wrapping "
      . "once every paragraph is reflowed independently by the editor.\n";
    my $code =
        "Index: foo.c\n"
      . "===================================================================\n"
      . "--- foo.c\n"
      . "+++ foo.c\n"
      . "@@ -1,2 +1,3 @@\n"
      . " int main(void)\n"
      . "+{ return 0; }\n" . " \n";
    my $orig = $first . "\n" . $second . "\n" . $third . "\n\n" . $code;
    write_raw( $path, $orig );

    my $s = vi_session( $work, $path );
    $s->pump(1.5);
    $s->write("\x1b");     # open the main menu
    $s->pump(0.6);
    $s->write("g");        # miscellaneous (item 7)
    $s->pump(0.6);
    $s->write("b");        # format whole document (item 2)
    $s->pump(2.5);
    $s->write(":wq\r");    # save the formatted buffer
    $s->pump(1.2);
    my $exited = $s->wait_exit;
    $s->close;

    return 0 unless $exited;
    my $result = read_raw($path);

    # every line fits the 72-column prose width
    for my $line ( split /\n/, $result ) {
        return 0 if length($line) > 72;
    }

    # the code block is classified as structured and left verbatim
    return 0 unless index( $result, $code ) >= 0;

    # paragraph boundaries and blank lines survive
    my $want_blank = grep { /^[ \t]*$/ } split /\n/, $orig,   -1;
    my $got_blank  = grep { /^[ \t]*$/ } split /\n/, $result, -1;
    return 0 unless $want_blank == $got_blank;

    # the short paragraph stays on its own line; nothing was merged
    return 0 unless $result =~ /\n\QA short second paragraph.\E\n/;

    return 1;
}

# Formatting a later single-line paragraph used to walk into the
# previous paragraph and merge the two.  The command must only touch the
# paragraph holding the cursor.
sub test_format_paragraph_stays_in_paragraph {
    my ($work) = @_;
    my $path = "$work/one-paragraph.txt";
    my $first =
        "The first paragraph must not be reformatted when the cursor is "
      . "in the second one, but it is long enough that a wrong merge would "
      . "show up clearly in the saved file.\n";
    my $second =
        "The second paragraph is the one the formatter is asked to "
      . "reflow and it is also long enough to require wrapping here.\n";
    my $orig = $first . "\n" . $second;
    write_raw( $path, $orig );

    my $s = vi_session( $work, $path );
    $s->pump(1.5);
    $s->write("jj");      # over the blank line to the second paragraph
    $s->pump(0.3);
    $s->write("\x1b");    # open the main menu
    $s->pump(0.6);
    $s->write("g");       # miscellaneous (item 7)
    $s->pump(0.6);
    $s->write("a");       # format paragraph (item 1)
    $s->pump(1.2);
    $s->write(":wq\r");
    $s->pump(1.2);
    my $exited = $s->wait_exit;
    $s->close;

    return 0 unless $exited;
    my $result = read_raw($path);

    # the first paragraph is untouched and still separated by a blank line
    return 0 unless substr( $result, 0,             length $first ) eq $first;
    return 0 unless substr( $result, length $first, 1 ) eq "\n";

    # the second paragraph was reflowed rather than merged into the first
    return 0 if index( $result, $second ) >= 0;
    return 1;
}

sub main {
    unless ( -x $EE ) {
        notok('ee binary exists');
        print "\npass: $PASS  fail: $FAIL\n";
        return 1;
    }
    ok('ee binary exists');

    my $tmpdir = $ENV{TMPDIR} // '/tmp';
    my $work   = tempdir(
        'ee-features.XXXXXX',
        DIR     => $tmpdir,
        CLEANUP => 1
    );

    check( 'vi: $ a append and :wq save',   test_vi_append_and_wq($work) );
    check( 'vi: dd deletes a line',         test_vi_delete_line($work) );
    check( 'vi: menu/config round trip',    test_config_roundtrip($work) );
    check( 'diff content is not corrupted', test_format_protects_diff($work) );
    check( 'whole document is reformatted paragraph by paragraph',
        test_format_whole_document($work) );
    check(
        'format paragraph never crosses a paragraph boundary',
        test_format_paragraph_stays_in_paragraph($work)
    );

    print "\npass: $PASS  fail: $FAIL\n";
    if ( $FAIL > 0 ) {
        my $names = join '', map { " $_" } @FAILED;
        print "failed tests:$names\n";
        return 1;
    }
    return 0;
}

exit main();
