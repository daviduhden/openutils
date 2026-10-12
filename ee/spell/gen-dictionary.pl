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
my $WORD_RE      = qr/^[a-z]+$|^[a-z]+'(?:s|t|d|ll|re|ve|m|clock)$/x;

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
    return if length($s) < 2;
    return substr( $s, -1 ) eq substr( $s, -2, 1 ) ? drop_last($s) : undef;
}

sub is_vowel {
    my ($c) = @_;
    return $c =~ /[aeiou]/x;
}

sub consonant_y {
    my ($w) = @_;
    return 0 if length($w) < 2;
    return substr( $w, -1 ) eq 'y' && !is_vowel( substr( $w, -2, 1 ) );
}

# True when $w ends in a doubled consonant eligible for -ed/-ing doubling.
sub doubles {
    my ($w) = @_;
    return 0 if length($w) < 2;
    my $last_ch = substr( $w, -1 );
    my $prev    = substr( $w, -2, 1 );
    return $last_ch !~ /[aeiouwxy]/x && is_vowel($prev);
}

# --- the single regular form for each suffix flag -------------------------

sub _regular_s {
    my ($w)     = @_;
    my $last_ch = substr( $w, -1 );
    my $prev    = length($w) >= 2 ? substr( $w, -2, 1 ) : '';
    return drop_last($w) . 'ies' if consonant_y($w);
    return $w . 'es'             if $last_ch =~ /[sxz]/x;
    return $w . 'es' if ends_with( $w, 'ch' ) || ends_with( $w, 'sh' );
    return $w . 's'  if $last_ch eq 'y' && is_vowel($prev);
    return           if $last_ch eq 'h';
    return $w . 's';
}

sub _regular_d {
    my ($w) = @_;
    my $last_ch = substr( $w, -1 );
    return $w . 'd'              if $last_ch eq 'e';
    return drop_last($w) . 'ied' if consonant_y($w);
    return                       if $last_ch eq 'y';
    return $w . 'ed';
}

sub _regular_g {
    my ($w) = @_;
    return drop_last($w) . 'ing' if substr( $w, -1 ) eq 'e';
    return $w . 'ing';
}

sub _regular_r {
    my ($w) = @_;
    my $last_ch = substr( $w, -1 );
    return $w . 'r'              if $last_ch eq 'e';
    return drop_last($w) . 'ier' if consonant_y($w);
    return                       if $last_ch eq 'y';
    return $w . 'er';
}

sub _regular_t {
    my ($w) = @_;
    my $last_ch = substr( $w, -1 );
    return $w . 'st'              if $last_ch eq 'e';
    return drop_last($w) . 'iest' if consonant_y($w);
    return                        if $last_ch eq 'y';
    return $w . 'est';
}

sub _regular_l {
    my ($w) = @_;
    return drop_last($w) . 'ily' if consonant_y($w);
    return                       if substr( $w, -1 ) eq 'y';
    return $w . 'ly';
}

my %REGULAR_FORM = (
    S => \&_regular_s,
    D => \&_regular_d,
    G => \&_regular_g,
    R => \&_regular_r,
    T => \&_regular_t,
    L => \&_regular_l,
);

# The single form that the .aff rule for $flag produces for $word.
sub regular_form {
    my ( $w, $flag ) = @_;
    return unless length($w);
    my $build = $REGULAR_FORM{$flag};
    return $build->($w) if $build;
    return;
}

# --- plausible non-rule forms (doubling, -le -> -ly, ...) -----------------

sub _variants_s {
    my ($w) = @_;
    my %out = ( $w . 's' => 1, $w . 'es' => 1 );
    $out{ drop_last($w) . 'ies' } = 1 if consonant_y($w);
    return %out;
}

sub _variants_d {
    my ($w) = @_;
    my %out = ( $w . 'ed' => 1, $w . 'd' => 1 );
    $out{ drop_last($w) . 'ied' } = 1 if consonant_y($w);
    $out{ $w . substr( $w, -1 ) . 'ed' } = 1 if doubles($w);
    $out{ $w . 'ked' } = 1 if ends_with( $w, 'c' );
    return %out;
}

sub _variants_g {
    my ($w) = @_;
    my %out = ( $w . 'ing' => 1 );
    $out{ drop_last($w) . 'ing' } = 1 if substr( $w, -1 ) eq 'e';
    $out{ substr( $w, 0, -2 ) . 'ying' }  = 1 if ends_with( $w, 'ie' );
    $out{ $w . substr( $w, -1 ) . 'ing' } = 1 if doubles($w);
    $out{ $w . 'king' } = 1 if ends_with( $w, 'c' );
    return %out;
}

sub _variants_r {
    my ($w) = @_;
    my %out = ( $w . 'er' => 1, $w . 'r' => 1 );
    $out{ drop_last($w) . 'ier' } = 1 if consonant_y($w);
    $out{ $w . substr( $w, -1 ) . 'er' } = 1 if doubles($w);
    return %out;
}

sub _variants_t {
    my ($w) = @_;
    my %out = ( $w . 'est' => 1, $w . 'st' => 1 );
    $out{ drop_last($w) . 'iest' } = 1 if consonant_y($w);
    $out{ $w . substr( $w, -1 ) . 'est' } = 1 if doubles($w);
    return %out;
}

sub _variants_l {
    my ($w) = @_;
    my %out = ( $w . 'ly' => 1 );
    $out{ substr( $w, 0, -2 ) . 'ly' } = 1 if ends_with( $w, 'le' );
    $out{ drop_last($w) . 'ily' } = 1 if consonant_y($w);
    $out{ drop_last($w) . 'ly' }  = 1 if ends_with( $w, 'll' );
    $out{ $w . 'ally' }           = 1 if ends_with( $w, 'ic' );
    return %out;
}

my %IRREGULAR_VARIANTS = (
    S => \&_variants_s,
    D => \&_variants_d,
    G => \&_variants_g,
    R => \&_variants_r,
    T => \&_variants_t,
    L => \&_variants_l,
);

sub irregular_variants {
    my ( $w, $flag ) = @_;
    my $build = $IRREGULAR_VARIANTS{$flag};
    return () unless $build;
    return $build->($w);
}

# --- possible base words for an inflected word, most likely first ---------

sub _base_ies {
    my ($w) = @_;
    my $stem = substr( $w, 0, -3 );
    return ( $stem . 'y', $stem, $stem . 'ie' );
}

sub _base_es {
    my ($w) = @_;
    return ( substr( $w, 0, -2 ), drop_last($w) );
}

sub _base_s {
    my ($w) = @_;
    return () if ends_with( $w, 'ss' );
    return drop_last($w);
}

sub _base_ied {
    my ($w) = @_;
    return substr( $w, 0, -3 ) . 'y';
}

sub _base_ed {
    my ($w)  = @_;
    my $stem = substr( $w, 0, -2 );
    my @out  = ( $stem, drop_last($w) );
    my $und  = undouble($stem);
    push @out, $und if defined $und;
    return @out;
}

sub _base_ing {
    my ($w)  = @_;
    my $stem = substr( $w, 0, -3 );
    my @out  = ( $stem, $stem . 'e' );
    my $und  = undouble($stem);
    push @out, $und if defined $und;
    return @out;
}

sub _base_ier {
    my ($w) = @_;
    return substr( $w, 0, -3 ) . 'y';
}

sub _base_iest {
    my ($w) = @_;
    return substr( $w, 0, -4 ) . 'y';
}

sub _base_er {
    my ($w)  = @_;
    my $stem = substr( $w, 0, -2 );
    my @out  = ( $stem, $stem . 'e' );
    my $und  = undouble($stem);
    push @out, $und if defined $und;
    return @out;
}

sub _base_est {
    my ($w)  = @_;
    my $stem = substr( $w, 0, -3 );
    my @out  = ( $stem, $stem . 'e' );
    my $und  = undouble($stem);
    push @out, $und if defined $und;
    return @out;
}

sub _base_ily {
    my ($w) = @_;
    return substr( $w, 0, -3 ) . 'y';
}

sub _base_ly {
    my ($w) = @_;
    return substr( $w, 0, -2 );
}

# Suffix, minimum word length, and candidate builder, applied in this order.
my @BASE_RULES = (
    [ 'ies',  4, \&_base_ies ],
    [ 'es',   3, \&_base_es ],
    [ 's',    2, \&_base_s ],
    [ 'ied',  4, \&_base_ied ],
    [ 'ed',   3, \&_base_ed ],
    [ 'ing',  4, \&_base_ing ],
    [ 'ier',  4, \&_base_ier ],
    [ 'iest', 5, \&_base_iest ],
    [ 'er',   3, \&_base_er ],
    [ 'est',  4, \&_base_est ],
    [ 'ily',  4, \&_base_ily ],
    [ 'ly',   3, \&_base_ly ],
);

sub base_candidates {
    my ($w) = @_;
    my @c = ($w);

    for my $pair (@PREFIXES) {
        my $pre = $pair->[1];
        push @c, substr( $w, length($pre) )
          if index( $w, $pre ) == 0 && length($w) > length($pre) + 1;
    }
    for my $rule (@BASE_RULES) {
        my ( $suffix, $minlen, $build ) = @$rule;
        next if length($w) <= $minlen;
        next unless ends_with( $w, $suffix );
        push @c, $build->($w);
    }

    my %seen;
    my @out;
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
        $line =~ s/^\s+//x;
        $line =~ s/\s+\z//x;
        $words{$line} = 1 if $line =~ $WORD_RE;
    }
    close $fh;
    return %words;
}

# --- assembling the entry table -------------------------------------------

sub _flags_for_base {
    my ( $base, $enable ) = @_;
    my %flags;
    for my $w ( keys %$base ) {
        $flags{$w} = {};
        for my $f (@SUFFIX_FLAGS) {
            my $form = regular_form( $w, $f );
            $flags{$w}{$f} = 1 if defined $form && $enable->{$form};
        }
        for my $pair (@PREFIXES) {
            my ( $pf, $pre ) = @$pair;
            $flags{$w}{$pf} = 1 if $enable->{ $pre . $w };
        }
    }
    return %flags;
}

sub _explicit_forms {
    my ( $base, $enable ) = @_;
    my %explicit;
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
    return %explicit;
}

sub _base_candidate {
    my ( $word, $base ) = @_;
    for my $candidate ( base_candidates($word) ) {
        return $candidate if $base->{$candidate};
    }
    return;
}

sub _form_is_generated {
    my ( $word, $cand, $flags ) = @_;
    for my $f ( keys %{ $flags->{$cand} } ) {
        my $form = regular_form( $cand, $f );
        return 1 if defined $form && $word eq $form;
    }
    for my $pair (@PREFIXES) {
        my ( $pf, $pre ) = @$pair;
        return 1 if $flags->{$cand}{$pf} && $word eq $pre . $cand;
    }
    return 0;
}

sub build {
    my ( $base, $enable ) = @_;
    my %flags    = _flags_for_base( $base, $enable );
    my %explicit = _explicit_forms( $base, $enable );

    for my $word ( keys %$enable ) {
        next if length($word) > 64;
        next if $base->{$word};
        my $cand = _base_candidate( $word, $base );
        next                 unless defined $cand;
        $explicit{$word} = 1 unless _form_is_generated( $word, $cand, \%flags );
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
        my $fl = join '', sort keys %{ $entries->{$w} };
        print {$fh} $w, ( length($fl) ? "/$fl" : '' ), "\n";
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
