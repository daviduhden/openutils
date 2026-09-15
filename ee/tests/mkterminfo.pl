#!/usr/bin/env perl

# Build a minimal terminfo entry for tests.  The strings may contain
# % escapes (including malformed ones) to exercise the conditional
# evaluator.  Terminfo binary layout (16-bit, little endian):
#   magic(2) names(2) booleans(2) numbers(2) strings(2) table(2)
#   names NUL separated + terminating NUL
#   booleans (1 byte each)
#   numbers  (2 bytes each, 0xFFFF = absent)
#   string offsets (2 bytes each, 0xFFFF = absent)
#   string table
#
# Requires only a Perl 5 core installation.

use strict;
use warnings;

my $CO = 0;      # number of columns
my $LI = 2;      # number of lines
my $CL = 5;      # clear screen
my $CM = 10;     # cursor motion
my $KS = 89;     # keypad on
my $KE = 88;     # keypad off
my $PC = 104;    # pad character
my $NS = 190;    # total number of string capabilities (see new_curse.c)

sub build {
    my ( $path, $clear, $cup, $pad ) = @_;
    $clear = "\033[H\033[2J" unless defined $clear;
    $cup   = "\033[%i%d;%dH" unless defined $cup;
    $pad   = "\0"            unless defined $pad;
    my @numbers = (0xFFFF) x 40;
    $numbers[$CO] = 80;
    $numbers[$LI] = 24;
    my @soff  = (0xFFFF) x $NS;
    my $table = '';
    my $add   = sub {
        my ( $idx, $s ) = @_;
        $soff[$idx] = length $table;
        $table .= $s . "\0";
        return;
    };
    $add->( $CL, $clear );
    $add->( $CM, $cup );
    $add->( $PC, $pad );
    my $names = "openutils-test|x|test terminal for ee tests\0";
    my $hdr   = pack( 'v6',
        282, length($names), 0, scalar(@numbers), $NS, length($table) );
    open my $fh, '>', $path or die "open $path: $!";
    binmode $fh;
    print {$fh} $hdr . $names;
    print {$fh} pack( 'v*', @numbers );
    print {$fh} pack( 'v*', @soff );
    print {$fh} $table;
    close $fh;
    return;
}

die "usage: $0 path [clear [cup [pad]]]\n" unless @ARGV;
build(@ARGV);
