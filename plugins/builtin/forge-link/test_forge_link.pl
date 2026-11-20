#!/usr/bin/env perl
# The plugin's pieces: perl test_forge_link.pl (make test runs it).
use strict;
use warnings;
use File::Temp qw(tempdir);
use FindBin;
use IO::Socket::UNIX;
use Test::More;

require "$FindBin::Bin/forge_link.pl";

# Remotes
is_deeply([parse_remote('git@github.com:owner/repo.git')], ['github.com', 'owner/repo'], 'the user@host:path form');
is_deeply([parse_remote('SSH://git@gitlab.example.org:2222/group/sub/proj.git')], ['gitlab.example.org', 'group/sub/proj'],
          'a URL with a port, the scheme in any case');
is_deeply([parse_remote('https://user@codeberg.org/me/thing/')], ['codeberg.org', 'me/thing'], 'https with a user');
is_deeply([parse_remote('/srv/git/local.git')], [], 'a local path is no web remote');

# Links
my $c = 'abc123';
is(web_url('github.com', 'o/r', $c, 'src/main.c', 10, 12), 'https://github.com/o/r/blob/abc123/src/main.c#L10-L12', 'GitHub');
is(web_url('github.com', 'o/r', $c, 'a b/ç.c', 3, 3), 'https://github.com/o/r/blob/abc123/a%20b/%C3%A7.c#L3', 'one line, encoded path');
is(web_url('gitlab.com', 'g/p', $c, 'x.py', 1, 4), 'https://gitlab.com/g/p/-/blob/abc123/x.py#L1-4', 'GitLab');
is(web_url('bitbucket.org', 't/r', $c, 'x.py', 5, 6), 'https://bitbucket.org/t/r/src/abc123/x.py#lines-5:6', 'Bitbucket');
is(web_url('codeberg.org', 'u/r', $c, 'x.py', 2, 2), 'https://codeberg.org/u/r/src/commit/abc123/x.py#L2', 'Codeberg');

# Selections
is_deeply([selected_lines('4 2 4 2')], [4, 4], 'no selection: the cursor line');
is_deeply([selected_lines('4 1 7 1')], [4, 6], 'ending at a line start leaves that line out');
is_deeply([selected_lines('4 3 7 5')], [4, 7], 'a selection');

# A repository with a remote, nothing pushed
my $repo = tempdir(CLEANUP => 1);
my $git = sub { system('git', '-C', $repo, '-c', 'user.name=t', '-c', 'user.email=t@t', @_) == 0 or die "git @_" };
$git->('init', '-q', '-b', 'main');
mkdir "$repo/src";
open my $fh, '>', "$repo/src/main.c" or die;
print $fh "int main(void)\n{\n  return 0;\n}\n";
close $fh;
$git->('add', '-A');
$git->('commit', '-qm', 'first');
$git->('remote', 'add', 'origin', 'git@github.com:me/proj.git');
chomp(my $head = `git -C $repo rev-parse HEAD`);
my ($url, $warning) = link_for("$repo/src/main.c", 2, 3);
is($url, "https://github.com/me/proj/blob/$head/src/main.c#L2-L3", 'the link from git');
like($warning, qr/not pushed yet/, 'warns when the commit is not pushed');
open $fh, '>', "$repo/new.c" or die;
close $fh;
ok(!eval { link_for("$repo/new.c", 1, 1) } && $@ =~ /not in git yet/, 'an untracked file');

# The whole command against a fake t-code: hello, the file, the selection, then copy and say
my $sock_path = "$repo/t-code.sock";
my $server = IO::Socket::UNIX->new(Type => Socket::SOCK_STREAM(), Local => $sock_path, Listen => 1) or die $!;
my $pid = fork // die;
if ($pid == 0) {
    $ENV{TCODE_SOCKET} = $sock_path;
    $ENV{TCODE_PLUGIN_TOKEN} = 'secret';
    exec $^X, "$FindBin::Bin/forge_link.pl", 'copy';
}
my $conn = $server->accept or die;
my @requests;
my %answers = ('hello' => "ok tapi_1 t-code test\n", 'get-file' => "ok $repo/src/main.c\n",
               'get-selection-range' => "ok 3 1 3 1\n");
local $/ = "\0";
while (my $request = <$conn>) {
    chomp $request;
    push @requests, $request;
    my ($command) = split / /, $request;
    print $conn $answers{$command} // "ok\n";
}
waitpid $pid, 0;
is($? >> 8, 0, 'the command ends well');
is($requests[0], 'hello tapi_1 secret', 'hello with the token');
is($requests[3], "copy https://github.com/me/proj/blob/$head/src/main.c#L3", 'the link goes to the clipboard');
like($requests[4], qr/^say Open on GitHub: link copied \(this commit is not pushed yet/, 'and the status bar says so');

done_testing();
