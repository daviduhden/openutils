#!/usr/bin/perl
#
# Regenerate ee's en_US.dic from public-domain word lists.
#
# This is a developer tool; it is not part of the build and is never run by
# `make`.  It performs no network access: download the source files first
# (see DICTIONARY-SOURCES.md) and pass their paths.
#
# Sources (all placed in the public domain; see DICTIONARY-SOURCES.md):
#
#   12dicts 6.0.2, American/2of12.txt  common American vocabulary
#   12dicts 6.0.2, American/3esl.txt   smaller, very common core
#   ENABLE2K (WORD.LST)                inflected forms used only to verify
#                                      candidate inflections
#
# The output is deterministic: entries are sorted and flags are emitted in
# a fixed order.  Only the affix transformations supported by ee/spell
# (one suffix or one prefix, no cross product) are used.
#
# Usage:
#   gen-dictionary.pl 2of12.txt 3esl.txt enable2k.txt en_US.dic

use strict;
use warnings;

my @SUFFIX_FLAGS = split //, "SDGRTL";
my @PREFIXES     = ( [ "U", "un" ], [ "P", "re" ], [ "X", "dis" ] );
my $WORD_RE      = qr/^[a-z]+$|^[a-z]+'(?:s|t|d|ll|re|ve|m|clock)$/;

sub ends_with {
    my ( $s, $suffix ) = @_;
    return 0 if length($s) < length($suffix);
    return substr( $s, -length($suffix) ) eq $suffix;
}

sub drop_last {
    my ($s) = @_;
    return substr( $s, 0, length($s) - 1 );
}

sub undouble {
    my ($s) = @_;
    return undef if length($s) < 2;
    return substr( $s, -1 ) eq substr( $s, -2, 1 ) ? drop_last($s) : undef;
}

sub is_vowel {
    my ($c) = @_;
    return $c =~ /[aeiou]/;
}

sub consonant_y {
    my ($w) = @_;
    return 0 if length($w) < 2;
    return substr( $w, -1 ) eq 'y' && !is_vowel( substr( $w, -2, 1 ) );
}

# The single form that the .aff rule for $flag produces for $word.
sub regular_form {
    my ( $w, $flag ) = @_;
    return undef unless length($w);
    my $last = substr( $w, -1 );
    my $prev = length($w) >= 2 ? substr( $w, -2, 1 ) : '';
    my $cy   = consonant_y($w);

    if ( $flag eq 'S' ) {
        return drop_last($w) . 'ies' if $cy;
        return $w . 'es'             if $last =~ /[sxz]/;
        return $w . 'es' if ends_with( $w, 'ch' ) || ends_with( $w, 'sh' );
        return $w . 's'  if $last eq 'y' && is_vowel($prev);
        return undef     if $last eq 'h';
        return $w . 's';
    }
    if ( $flag eq 'D' ) {
        return $w . 'd'              if $last eq 'e';
        return drop_last($w) . 'ied' if $cy;
        return undef                 if $last eq 'y';
        return $w . 'ed';
    }
    if ( $flag eq 'G' ) {
        return drop_last($w) . 'ing' if $last eq 'e';
        return $w . 'ing';
    }
    if ( $flag eq 'R' ) {
        return $w . 'r'              if $last eq 'e';
        return drop_last($w) . 'ier' if $cy;
        return undef                 if $last eq 'y';
        return $w . 'er';
    }
    if ( $flag eq 'T' ) {
        return $w . 'st'              if $last eq 'e';
        return drop_last($w) . 'iest' if $cy;
        return undef                  if $last eq 'y';
        return $w . 'est';
    }
    if ( $flag eq 'L' ) {
        return drop_last($w) . 'ily' if $cy;
        return undef                 if $last eq 'y';
        return $w . 'ly';
    }
    return undef;
}

# Plausible non-rule forms (doubling, -le -> -ly, ...).
sub irregular_variants {
    my ( $w, $flag ) = @_;
    my %out;
    my $last = substr( $w, -1 );
    my $prev = length($w) >= 2 ? substr( $w, -2, 1 ) : '';
    my $dbl  = length($w) >= 2 && $last !~ /[aeiouwxy]/ && is_vowel($prev);
    my $cy   = consonant_y($w);

    if ( $flag eq 'S' ) {
        $out{ $w . 's' }              = 1;
        $out{ $w . 'es' }             = 1;
        $out{ drop_last($w) . 'ies' } = 1 if $cy;
    }
    elsif ( $flag eq 'D' ) {
        $out{ $w . 'ed' }             = 1;
        $out{ $w . 'd' }              = 1;
        $out{ drop_last($w) . 'ied' } = 1 if $cy;
        $out{ $w . $last . 'ed' }     = 1 if $dbl;
        $out{ $w . 'ked' }            = 1 if ends_with( $w, 'c' );
    }
    elsif ( $flag eq 'G' ) {
        $out{ $w . 'ing' }            = 1;
        $out{ drop_last($w) . 'ing' } = 1 if $last eq 'e';
        $out{ substr( $w, 0, -2 ) . 'ying' } = 1 if ends_with( $w, 'ie' );
        $out{ $w . $last . 'ing' } = 1 if $dbl;
        $out{ $w . 'king' }        = 1 if ends_with( $w, 'c' );
    }
    elsif ( $flag eq 'R' ) {
        $out{ $w . 'er' }             = 1;
        $out{ $w . 'r' }              = 1;
        $out{ drop_last($w) . 'ier' } = 1 if $cy;
        $out{ $w . $last . 'er' }     = 1 if $dbl;
    }
    elsif ( $flag eq 'T' ) {
        $out{ $w . 'est' }             = 1;
        $out{ $w . 'st' }              = 1;
        $out{ drop_last($w) . 'iest' } = 1 if $cy;
        $out{ $w . $last . 'est' }     = 1 if $dbl;
    }
    elsif ( $flag eq 'L' ) {
        $out{ $w . 'ly' } = 1;
        $out{ substr( $w, 0, -2 ) . 'ly' } = 1 if ends_with( $w, 'le' );
        $out{ drop_last($w) . 'ily' } = 1 if $cy;
        $out{ drop_last($w) . 'ly' }  = 1 if ends_with( $w, 'll' );
        $out{ $w . 'ally' }           = 1 if ends_with( $w, 'ic' );
    }
    return %out;
}

# Possible base words for an inflected word, most likely first.
sub base_candidates {
    my ($w) = @_;
    my @c = ($w);
    my %seen;
    my @out;

    for my $pair (@PREFIXES) {
        my $pre = $pair->[1];
        if ( index( $w, $pre ) == 0 && length($w) > length($pre) + 1 ) {
            push @c, substr( $w, length($pre) );
        }
    }
    if ( ends_with( $w, 'ies' ) && length($w) > 4 ) {
        push @c, substr( $w, 0, -3 ) . 'y', substr( $w, 0, -3 ),
          substr( $w, 0, -3 ) . 'ie';
    }
    if ( ends_with( $w, 'es' ) && length($w) > 3 ) {
        push @c, substr( $w, 0, -2 ), drop_last($w);
    }
    if ( ends_with( $w, 's' ) && !ends_with( $w, 'ss' ) && length($w) > 2 ) {
        push @c, drop_last($w);
    }
    if ( ends_with( $w, 'ied' ) && length($w) > 4 ) {
        push @c, substr( $w, 0, -3 ) . 'y';
    }
    if ( ends_with( $w, 'ed' ) && length($w) > 3 ) {
        my $und = undouble( substr( $w, 0, -2 ) );
        push @c, substr( $w, 0, -2 ), drop_last($w);
        push @c, $und if defined $und;
    }
    if ( ends_with( $w, 'ing' ) && length($w) > 4 ) {
        my $und = undouble( substr( $w, 0, -3 ) );
        push @c, substr( $w, 0, -3 ), substr( $w, 0, -3 ) . 'e';
        push @c, $und if defined $und;
    }
    if ( ends_with( $w, 'ier' ) && length($w) > 4 ) {
        push @c, substr( $w, 0, -3 ) . 'y';
    }
    if ( ends_with( $w, 'iest' ) && length($w) > 5 ) {
        push @c, substr( $w, 0, -4 ) . 'y';
    }
    if ( ends_with( $w, 'er' ) && length($w) > 3 ) {
        my $und = undouble( substr( $w, 0, -2 ) );
        push @c, substr( $w, 0, -2 ), substr( $w, 0, -2 ) . 'e';
        push @c, $und if defined $und;
    }
    if ( ends_with( $w, 'est' ) && length($w) > 4 ) {
        my $und = undouble( substr( $w, 0, -3 ) );
        push @c, substr( $w, 0, -3 ), substr( $w, 0, -3 ) . 'e';
        push @c, $und if defined $und;
    }
    if ( ends_with( $w, 'ily' ) && length($w) > 4 ) {
        push @c, substr( $w, 0, -3 ) . 'y';
    }
    if ( ends_with( $w, 'ly' ) && length($w) > 3 ) {
        push @c, substr( $w, 0, -2 );
    }
    for my $item (@c) {
        next if !length($item) || $seen{$item}++;
        push @out, $item;
    }
    return @out;
}

sub read_words {
    my ($path) = @_;
    my %words;
    open my $fh, '<', $path or die "open $path: $!\n";
    while ( my $line = <$fh> ) {
        $line =~ s/^\s+//;
        $line =~ s/\s+\z//;
        $words{$line} = 1 if $line =~ $WORD_RE;
    }
    close $fh;
    return %words;
}

sub build {
    my ( $base, $enable ) = @_;
    my ( %flags, %explicit );

    for my $w ( keys %$base ) {
        $flags{$w} = {};
        for my $f (@SUFFIX_FLAGS) {
            my $form = regular_form( $w, $f );
            $flags{$w}{$f} = 1
              if defined $form && $enable->{$form};
        }
        for my $pair (@PREFIXES) {
            my ( $pf, $pre ) = @$pair;
            $flags{$w}{$pf} = 1 if $enable->{ $pre . $w };
        }
    }

    for my $w ( keys %$base ) {
        for my $f (@SUFFIX_FLAGS) {
            my $regular = regular_form( $w, $f );
            my %variant = irregular_variants( $w, $f );
            for my $v ( keys %variant ) {
                next              if defined $regular && $v eq $regular;
                $explicit{$v} = 1 if $enable->{$v};
            }
        }
    }

    for my $word ( keys %$enable ) {
        next if length($word) > 64;
        my $cand;
        for my $candidate ( base_candidates($word) ) {
            if ( $base->{$candidate} ) {
                $cand = $candidate;
                last;
            }
        }
        next unless defined $cand;
        next if $base->{$word};
        my $generated = 0;
        for my $f ( keys %{ $flags{$cand} } ) {
            my $form = regular_form( $cand, $f );
            if ( defined $form && $word eq $form ) {
                $generated = 1;
                last;
            }
        }
        if ( !$generated ) {
            for my $pair (@PREFIXES) {
                my ( $pf, $pre ) = @$pair;
                if ( $flags{$cand}{$pf} && $word eq $pre . $cand ) {
                    $generated = 1;
                    last;
                }
            }
        }
        $explicit{$word} = 1 unless $generated;
    }

    my %entries;
    for my $w ( keys %$base ) {
        $entries{$w} = $flags{$w};
    }
    for my $w ( keys %explicit ) {
        $entries{$w} = {} unless exists $entries{$w};
    }
    return %entries;
}

sub write_dic {
    my ( $entries, $path ) = @_;
    my @words = sort { $a cmp $b } keys %$entries;
    open my $fh, '>', $path or die "open $path: $!\n";
    binmode $fh;
    print {$fh} scalar(@words), "\n";
    for my $w (@words) {
        my @fl = sort keys %{ $entries->{$w} };
        if (@fl) {
            print {$fh} $w, '/', join( '', @fl ), "\n";
        }
        else {
            print {$fh} $w, "\n";
        }
    }
    close $fh;
    return scalar @words;
}

if ( @ARGV != 4 ) {
    die "usage: $0 2of12.txt 3esl.txt enable2k.txt en_US.dic\n";
}
my ( $two, $three, $enable_path, $output ) = @ARGV;
my %base    = ( read_words($two), read_words($three) );
my %enable  = read_words($enable_path);
my %entries = build( \%base, \%enable );
my $total   = write_dic( \%entries, $output );
my $flagged = grep { scalar keys %{ $entries{$_} } } keys %entries;
print STDERR "base=", scalar( keys %base ), " enable=", scalar( keys %enable ),
  " entries=$total flagged=$flagged\n";
