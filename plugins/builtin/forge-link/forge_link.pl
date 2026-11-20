#!/usr/bin/env perl
# Open on GitHub / GitLab for t-code, in Perl 5 (core modules only).
#
#   copy  puts the web link of the cursor's line (or the selected lines) on the clipboard:
#         https://github.com/owner/repo/blob/<commit>/src/main.c#L10-L12
#   open  opens that link in the browser (xdg-open)
#
# The link names the commit (it keeps pointing at the same text), from the remote the branch
# tracks (else origin). GitHub, GitLab (any host with "gitlab" in its name), Codeberg / Gitea /
# Forgejo, Bitbucket; other hosts get GitHub's form. It speaks tapi_1 over $TCODE_SOCKET (t-code's
# docs/plugins.md).
use strict;
use warnings;
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use IO::Socket::UNIX;

# Talking to t-code -------------------------------------------------------------------------------

package Tcode {
    sub connect {
        my ($class, $path, $token) = @_;
        my $sock = IO::Socket::UNIX->new(Type => Socket::SOCK_STREAM(), Peer => $path)
          or die "cannot reach t-code: $!\n";
        binmode $sock, ':raw';
        my $self = bless { sock => $sock }, $class;
        $self->request('hello', $token ? "tapi_1 $token" : 'tapi_1');
        return $self;
    }

    # One request, ended by a NUL; the value after "ok". An "error <code> <message>" dies with
    # "<code>: <message>"; "closed: ..." when t-code went away.
    sub request {
        my ($self, $command, $argument) = @_;
        $argument //= '';
        my $message = "$command $argument";
        utf8::encode($message) if utf8::is_utf8($message);
        print { $self->{sock} } $message, "\0" or die "closed: t-code went away\n";
        my $line = readline $self->{sock};
        die "closed: t-code went away\n" unless defined $line;
        chomp $line;
        return '' if $line eq 'ok';
        return $1 if $line =~ /^ok (.*)$/s;
        $line =~ s/^error //;
        die "$line\n";
    }

    sub say {
        my ($self, $text) = @_;
        eval { $self->request('say', $text) };
    }
}

# Remotes and links -----------------------------------------------------------------------------

# (host, path) of a remote URL: git@host:owner/repo.git, ssh://git@host:22/owner/repo,
# https://user@host/owner/repo.git; undef for what is not a web host (a local path).
sub parse_remote {
    my ($url) = @_;
    return unless defined $url;
    my ($host, $path);
    if ($url =~ m{^(?:ssh|git|https?)://(?:[^@/]+@)?([^/:]+)(?::\d+)?/(.+)$}i) { # schemes ignore case
        ($host, $path) = ($1, $2);
    } elsif ($url =~ m{^(?:[^@/]+@)?([^/:]+):(?!/)(.+)$}) {
        ($host, $path) = ($1, $2);
    } else {
        return;
    }
    $path =~ s{\.git/?$}{};
    $path =~ s{/+$}{};
    return ($host, $path);
}

# A path's segments, each percent-encoded where a URL needs it.
sub encode_path {
    my ($path) = @_;
    utf8::encode($path) if utf8::is_utf8($path);
    return join '/', map { (my $s = $_) =~ s/([^A-Za-z0-9._~!\$&'()*+,;=:@-])/sprintf('%%%02X', ord $1)/ge; $s }
      split m{/}, $path, -1;
}

# The web link of lines first..last of file (relative to the repository) at commit.
sub web_url {
    my ($host, $repo, $commit, $file, $first, $last) = @_;
    my $base = "https://$host/$repo";
    my $path = encode_path($file);
    my $span = $first == $last;
    if ($host =~ /gitlab/i) {
        return "$base/-/blob/$commit/$path#L$first" . ($span ? '' : "-$last");
    }
    if ($host =~ /(?:^|\.)bitbucket\.org$/i) {
        return "$base/src/$commit/$path#lines-$first" . ($span ? '' : ":$last");
    }
    if ($host =~ /(?:codeberg|gitea|forgejo)/i) {
        return "$base/src/commit/$commit/$path#L$first" . ($span ? '' : "-L$last");
    }
    return "$base/blob/$commit/$path#L$first" . ($span ? '' : "-L$last");
}

# The lines a selection covers: "l1 c1 l2 c2" (from get-selection-range); a selection that ends at
# the start of a line does not take that line.
sub selected_lines {
    my ($range) = @_;
    my ($l1, $c1, $l2, $c2) = split ' ', $range;
    return (1, 1) unless defined $c2;
    $l2-- if $l2 > $l1 && $c2 == 1;
    return ($l1, $l2);
}

# Running git ---------------------------------------------------------------------------------------

sub git {
    my ($dir, @args) = @_;
    # git's own complaints ("no upstream configured") are answers here, not news for the log
    open(my $stderr, '>&', \*STDERR) or return;
    open(STDERR, '>', '/dev/null');
    my $started = open(my $out, '-|', 'git', '-C', $dir, @args);
    open(STDERR, '>&', $stderr);
    return unless $started;
    local $/;
    my $text = <$out>;
    close $out or return;
    $text //= '';
    $text =~ s/\n\z//; # chomp would not: $/ is undef here
    return $text;
}

# (link, warning) for lines first..last of file; dies with a message for the user when there is none.
sub link_for {
    my ($file, $first, $last) = @_;
    $file = abs_path($file) // $file; # the same spelling as git's top folder, links resolved
    my $dir = dirname($file);
    my $top = git($dir, 'rev-parse', '--show-toplevel');
    die "not in a git repository\n" unless defined $top && length $top;
    $top = abs_path($top) // $top;
    my $relative = substr($file, length($top) + 1);
    die "this file is not in git yet\n" unless defined git($top, 'ls-files', '--error-unmatch', '--', $relative);
    my $commit = git($top, 'rev-parse', 'HEAD') or die "the repository has no commit yet\n";
    my $upstream = git($top, 'rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{upstream}');
    my $remote = defined $upstream && $upstream =~ m{^([^/]+)/} ? $1 : 'origin';
    my ($host, $repo) = parse_remote(git($top, 'remote', 'get-url', $remote));
    die "no web remote (\"$remote\" is not a GitHub, GitLab or similar address)\n" unless $host;
    my $pushed = git($top, 'branch', '-r', '--contains', $commit);
    my $warning = defined $pushed && length $pushed ? '' : ' (this commit is not pushed yet: the link works once it is)';
    return (web_url($host, $repo, $commit, $relative, $first, $last), $warning);
}

# The plugin ----------------------------------------------------------------------------------------

sub run {
    my ($command) = @_;
    my $t = Tcode->connect($ENV{TCODE_SOCKET} // die("no \$TCODE_SOCKET: t-code runs this plugin\n"),
        $ENV{TCODE_PLUGIN_TOKEN} // '');
    my $file = $t->request('get-file');
    if ($file eq '') {
        $t->say('Open on GitHub: open a file first');
        return;
    }
    my ($first, $last) = selected_lines($t->request('get-selection-range'));
    my ($url, $warning) = eval { link_for($file, $first, $last) };
    if (!$url) {
        (my $why = $@) =~ s/\n$//;
        $t->say("Open on GitHub: $why");
        return;
    }
    if ($command eq 'open') {
        my $pid = fork;
        if (defined $pid && $pid == 0) {
            open STDOUT, '>', '/dev/null';
            open STDERR, '>', '/dev/null';
            exec 'xdg-open', $url or exit 127;
        }
        $t->say("Open on GitHub: opening the link$warning");
    } else {
        $t->request('copy', $url);
        $t->say("Open on GitHub: link copied$warning");
    }
}

unless (caller) {
    my $command = $ARGV[0] // '';
    if ($command ne 'copy' && $command ne 'open') {
        print STDERR "forge-link: unknown command \"$command\"\n";
        exit 1;
    }
    eval { run($command); 1 } or do {
        exit 0 if $@ =~ /^closed:/;
        print STDERR "forge-link: $command: $@";
        exit 1;
    };
}

1;
