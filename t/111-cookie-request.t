# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

# A stored page, or its Markdown copy, is served to a request carrying a
# Cookie header only when the stored page is marked public or s-maxage.
# Without a cookie the same entries are served as before.  t/seed-111.sh
# stores the entries; every miss goes to the /origin location.
#
# The prologue plays the optimizer: a shared config with agent optimization
# on (so an `Accept: text/markdown` request may select the Markdown copy) and
# a notify socket nothing listens on.

use File::Path qw(make_path);

open(my $sc, '<', 'src/worker/shared_config.h') or die "shared_config.h: $!";
our $gen;
while (<$sc>) {
    if (/kCacheDirGeneration\s*=\s*(\d+)\s*;/) {
        $gen = $1;
        last;
    }
}
close($sc);
die "kCacheDirGeneration not found" unless $gen;

BEGIN {
    system("bash t/seed-111.sh") == 0
        or die "Cache seeding failed";
}

{
    my $dir = "/tmp/pscookie-111";
    make_path($dir);
    open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
    print $fh "version=1\ncache_dir_generation=$gen\n"
        . "socket_path=$dir/absent.sock\n"
        . "agent_optimize_entitled=true\n";
    close($fh);
}

# The origin answers with no-store so that no miss below is recorded over a
# seeded entry: every test reads the seeds as stored.
our $config = <<'_EOC_';
server_name localhost;
pagespeed on;
pagespeed_cache_path /tmp/pscookie-111/cache.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "no-store";
    return 200 "from origin";
}
location ~ ^/(plain|public|smaxage|mustrev|md-plain|md-public)$ {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
_EOC_

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: a stored page without a sharing mark is served without a cookie
--- config eval: $::config
--- request
GET /plain
--- response_body chomp
<html><body>stored plain</body></html>
--- response_headers
X-PageSpeed: HIT


=== TEST 2: a stored page without a sharing mark is not served to a cookie
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /plain
--- response_body chomp
from origin
--- response_headers
X-PageSpeed: MISS


=== TEST 3: the obsolete Cookie2 header counts as a cookie
--- config eval: $::config
--- more_headers
Cookie2: $Version="1"
--- request
GET /plain
--- response_body chomp
from origin


=== TEST 4: a public page is served to a cookie
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /public
--- response_body chomp
<html><body>stored public</body></html>
--- response_headers
X-PageSpeed: HIT


=== TEST 5: an s-maxage page is served to a cookie
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /smaxage
--- response_body chomp
<html><body>stored smaxage</body></html>
--- response_headers
X-PageSpeed: HIT


=== TEST 6: must-revalidate does not mark a page shareable
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /mustrev
--- response_body chomp
from origin


=== TEST 7: a Markdown copy is served to an agent without a cookie
--- config eval: $::config
--- more_headers
Accept: text/markdown
--- request
GET /md-plain
--- response_body chomp
# markdown /md-plain


=== TEST 8: a Markdown copy of an unmarked page is not served to a cookie
--- config eval: $::config
--- more_headers
Accept: text/markdown
Cookie: session=one
--- request
GET /md-plain
--- response_body chomp
from origin


=== TEST 9: a Markdown copy of a public page is served to a cookie
--- config eval: $::config
--- more_headers
Accept: text/markdown
Cookie: session=one
--- request
GET /md-public
--- response_body chomp
# markdown /md-public
