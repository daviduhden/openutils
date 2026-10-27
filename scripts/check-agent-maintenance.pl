#!/usr/bin/env perl

use strict;
use warnings;
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use File::Find qw(find);
use File::Spec;
use Getopt::Long qw(GetOptions);

my $root = abs_path(File::Spec->catdir(dirname(__FILE__), File::Spec->updir));
my $base;
GetOptions('base=s' => \$base) or die "usage: perl scripts/check-agent-maintenance.pl [--base REF]\n";

sub git_output {
	my (@args) = @_;
	open(my $git, '-|', 'git', @args) or die "cannot run git: $!\n";
	binmode($git);
	local $/;
	my $output = <$git> // '';
	close($git) or die "git @args failed\n";
	return $output;
}

sub relative_path {
	my ($path) = @_;
	$path =~ s{^\Q$root\E/}{};
	return $path;
}

sub get_changed_paths {
	my @names;
	if (defined $base) {
		@names = split(/\0/, git_output('diff', '--name-only', '--diff-filter=ACDMRT', '-z', "$base...HEAD"));
	} else {
		@names = split(/\0/, git_output('diff', '--name-only', '--diff-filter=ACDMRT', '-z', 'HEAD'));
		push(@names, split(/\0/, git_output('ls-files', '--others', '--exclude-standard', '-z')));
	}
	return { map { $_ => 1 } grep { length($_) } @names };
}

sub is_instruction_path {
	my ($rel) = @_;
	return 1 if $rel =~ m{(?:^|/)AGENTS\.md$};
	return 1 if $rel =~ m{^\.github/agents/[^/]+\.agent\.md$};
	return 1 if $rel =~ m{^\.(?:agents|github|claude)/skills/.+/SKILL\.md$};
	return 1 if $rel =~ m{^\.github/instructions/.+\.md$};
	return 1 if $rel =~ m{^\.cursor/rules/.+};
	return 1 if $rel =~ m{^(?:\.github/copilot-instructions\.md|(?:.*/)?CLAUDE\.md|\.cursorrules)$};
	return 0;
}

my @instruction_files;
find({
	no_chdir => 1,
	preprocess => sub { grep { $_ ne '.git' && $_ ne '__pycache__' } @_ },
	wanted => sub {
		return unless -f $_;
		my $rel = relative_path($File::Find::name);
		push(@instruction_files, $File::Find::name) if is_instruction_path($rel);
	},
}, $root);

sub slurp_file {
	my ($path) = @_;
	open(my $fh, '<:encoding(UTF-8)', $path) or return;
	local $/;
	return <$fh> // '';
}

my @errors;
sub validate_instruction {
	my ($path) = @_;
	my $rel = relative_path($path);
	my $content = slurp_file($path);
	if (!defined $content) {
		push(@errors, "$rel: cannot read UTF-8 instruction file");
		return;
	}
	if ($rel =~ m{(?:^|/)AGENTS\.md$}) {
		push(@errors, "$rel: missing required 'Continuous Agent & Skills Maintenance' section")
			unless $content =~ /^## Continuous Agent & Skills Maintenance\s*$/m;
	} elsif ($rel =~ m{/SKILL\.md$}) {
		my ($front) = $content =~ /\A---\r?\n(.*?)\r?\n---\r?\n/s;
		if (!defined($front) || $front !~ /^name:\s*\S+/m || $front !~ /^description:\s*\S+/m) {
			push(@errors, "$rel: frontmatter must contain name and description");
		}
		push(@errors, "$rel: missing required 'Maintenance & Continuous Learning' section")
			unless $content =~ /^## Maintenance & Continuous Learning\s*$/m;
	} elsif (is_instruction_path($rel)) {
		push(@errors, "$rel: missing required 'Post-Change Knowledge Synchronization' section")
			unless $content =~ /^## Post-Change Knowledge Synchronization\s*$/m;
	}

	while ($content =~ /(?<!!)\[[^\]]*\]\(([^)]+)\)/g) {
		my $target = $1;
		$target =~ s/^\s+|\s+$//g;
		$target =~ s/^<|>$//g;
		next if $target eq '' || $target =~ m{^(?:https?://|mailto:|#)};
		my ($local) = split(/[?#]/, $target, 2);
		next if !defined($local) || $local eq '';
		my $resolved = File::Spec->rel2abs($local, dirname($path));
		push(@errors, "$rel: broken local Markdown reference '$target'") unless -e $resolved;
	}

	while ($content =~ m{((?:\.agents/skills/[^\s`)'"<>]+/SKILL\.md)|(?:\.github|\.claude)/skills/[^\s`)'"<>]+/SKILL\.md|(?:[\w./-]+/)?AGENTS\.md|\.github/agents/[\w.-]+\.agent\.md)}g) {
		my $ref = $1;
		next if $ref =~ /[*?\[]/ || $ref =~ /<[^>]+>/; # Optional patterns and documented placeholders.
		push(@errors, "$rel: broken agent/skill reference '$ref'") unless -e File::Spec->catfile($root, $ref);
	}
}

sub changed_instruction_delta {
	my ($rel) = @_;
	my @args = ('diff', '--no-ext-diff', '--unified=0');
	push(@args, "$base...HEAD") if defined $base;
	push(@args, 'HEAD') unless defined $base;
	push(@args, '--', $rel);
	my $diff = eval { git_output(@args) } // '';
	if ($diff eq '' && !-e File::Spec->catfile($root, $rel)) {
		return 0;
	} elsif ($diff eq '' && !git_file_tracked($rel)) {
		$diff = join('', map { "+$_" } split(/\n/, slurp_file(File::Spec->catfile($root, $rel)) // ''));
	}
	my $size = 0;
	for my $line (split(/\n/, $diff)) {
		next unless $line =~ /^[+-](?![+-])/;
		$line =~ s/^[+-]\s*//;
		next if $line eq '' || $line =~ /^#/;
		$size += length($line);
	}
	return $size;
}

sub git_file_tracked {
	my ($rel) = @_;
	open(my $git, '-|', 'git', 'ls-files', '--error-unmatch', '--', $rel) or return 0;
	while (<$git>) { }
	return close($git) ? 1 : 0;
}

my $changed = eval { get_changed_paths() };
if (!defined $changed) {
	my $error = $@ || 'could not identify Git changes';
	$error =~ s/\s+$//;
	print STDERR "agent maintenance: cannot identify Git changes: $error\n";
	exit 2;
}

validate_instruction($_) for @instruction_files;
my %known = map { relative_path($_) => 1 } @instruction_files;
my @changed_instructions = grep { $known{$_} } keys %{$changed};
if (keys(%{$changed}) && !@changed_instructions) {
	push(@errors, 'repository files changed, but no AGENTS.md, agent, or SKILL.md was updated');
}
if (@changed_instructions) {
	my $delta = 0;
	$delta += changed_instruction_delta($_) for @changed_instructions;
	push(@errors, 'agent/skill changes are empty or too small to demonstrate knowledge maintenance (need at least 40 changed content characters)')
		if $delta < 40;
}

if (@errors) {
	print STDERR "agent maintenance: FAIL\n";
	print STDERR "  - $_\n" for @errors;
	print STDERR "Review relevant instructions, make a substantive update, and rerun this check.\n";
	exit 1;
}
printf("agent maintenance: PASS (%d changed path(s), %d instruction file(s) checked)\n",
	 scalar(keys(%{$changed})), scalar(@instruction_files));
print "Structural and path checks passed; semantic relevance still requires review.\n";
exit 0;
