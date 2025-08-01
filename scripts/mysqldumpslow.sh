#!@PERL_PATH@

# Copyright (c) 2000, 2017, Oracle and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; version 2
# of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free
# Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
# MA 02110-1335  USA

# mysqldumpslow - parse and summarize the MySQL slow query log

# Original version by Tim Bunce, sometime in 2000.
# Further changes by Tim Bunce, 8th March 2001.
# Handling of strings with \ and double '' by Monty 11 Aug 2001.

use strict;
use Getopt::Long;

warn "$0: Deprecated program name. It will be removed in a future release, use 'mariadb-dumpslow' instead\n"
  if $0 =~ m/mysqldumpslow$/;

# t=time, l=lock time, r=rows, a=rows affected
# at, al, ar and aa are the corresponding averages

my %opt = (
    s => 'at',
    h => '*',
);

GetOptions(\%opt,
    'v|verbose+',# verbose
    'help+',	# write usage info
    'd|debug+',	# debug
    's=s',	# what to sort by (aa, ae, al, ar, at, a, c, e, l, r, t)
    'r!',	# reverse the sort order (largest last instead of first)
    't=i',	# just show the top n queries
    'a!',	# don't abstract all numbers to N and strings to 'S'
    'n=i',	# abstract numbers with at least n digits within names
    'g=s',	# grep: only consider stmts that include this string
    'h=s',	# hostname/basename of db server for *-slow.log filename (can be wildcard)
    'i=s',	# name of server instance (if using mysql.server startup script)
    'l!',	# don't subtract lock time from total time
    'json|j!', # print as a JSON-formatted string
) or usage("bad option");

$opt{'help'} and usage();

# check if JSON module is available
if ($opt{json}) {
    eval {
        require JSON;
        JSON->import();
        1;
    } or do {
        die "JSON module not found. Please install the JSON module to use --json option.\n";
    };
}

unless (@ARGV) {
    my $defaults   = `my_print_defaults --mysqld`;

    my $datadir = ($defaults =~ m/--datadir=(.*)/g)[-1];
    if (!$datadir or $opt{i}) {
	# determine the datadir from the instances section of /etc/my.cnf, if any
	my $instances  = `my_print_defaults instances`;
	die "Can't determine datadir from 'my_print_defaults instances' output: $defaults"
	    unless $instances;
	my @instances = ($instances =~ m/^--(\w+)-/mg);
	die "No -i 'instance_name' specified to select among known instances: @instances.\n"
	    unless $opt{i};
	die "Instance '$opt{i}' is unknown (known instances: @instances)\n"
	    unless grep { $_ eq $opt{i} } @instances;
	$datadir = ($instances =~ m/--$opt{i}-datadir=(.*)/g)[-1]
	    or die "Can't determine --$opt{i}-datadir from 'my_print_defaults instances' output: $instances";
	warn "datadir=$datadir\n" if $opt{v};
    }

    my $slowlog = ($defaults =~ m/--log[-_]slow[-_]queries=(.*)/g)[-1];
    if (!$slowlog)
    {
      $slowlog = ($defaults =~ m/--slow[-_]query[-_]log[-_]file=(.*)/g)[-1];
    }
    if ( $slowlog )
    {
        @ARGV = ($slowlog);
        die "Can't find '$slowlog'\n" unless @ARGV;
    }
    else
    {
      if (!$opt{h})
      {
        $opt{h}= ($defaults =~ m/--log[-_]basename=(.*)/g)[-1];
      }
      @ARGV = <$datadir/$opt{h}-slow.log>;
      die "Can't find '$datadir/$opt{h}-slow.log'\n" unless @ARGV;
    }
}

warn "\nReading mysql slow query log from @ARGV\n";

my @pending;
my %stmt;
$/ = ";\n#";		# read entire statements using paragraph mode
while (<>) {
    warn "[[$_]]\n" if $opt{d};	# show raw paragraph being read

    # remove fluff that mysqld writes to log when it (re)starts:
    s!^.*Version.*started with:.*\n!!mg;
    s!^Tcp port: \d+  Unix socket: \S+\n!!mg;
    s!^Time.*Id.*Command.*Argument.*\n!!mg;
    # if there is only header info, skip
    if ($_ eq '') {
        next;
    }

    s/^#? Time: \d{6}\s+\d+:\d+:\d+.*\n//;
    my ($user,$host) = s/^#? User\@Host:\s+(\S+)\s+\@\s+(\S+).*\n// ? ($1,$2) : ('','');

    s/^# Thread_id: [0-9]+\s+Schema: .*\s+QC_hit:.*[^\n]+\n//;
    s/^# Query_time: ([0-9.]+)\s+Lock_time: ([0-9.]+)\s+Rows_sent: ([0-9.]+)\s+Rows_examined: ([0-9.]+).*\n//;
    my ($t, $l, $r, $e) = ($1, $2, $3, $4);
    s/^# Rows_affected: ([0-9.]+).*\n//;
    my ($a) = ($1);

    $t -= $l unless $opt{l};

    # Remove optimizer info
    s!^# QC_Hit: \S+\s+Full_scan: \S+\s+Full_join: \S+\s+Tmp_table: \S+\s+Tmp_table_on_disk: \S+[^\n]+\n!!mg;
    s!^# Filesort: \S+\s+Filesort_on_disk: \S+[^\n]+\n!!mg;
    s!^# Full_scan: \S+\s+Full_join: \S+[^\n]+\n!!mg;

    s!^SET timestamp=\d+;\n!!m; # remove the redundant timestamp that is always added to each query
    s!^use \w+;\n!!m; # not consistently added

    s/^[ 	]*\n//mg;	# delete blank lines
    s/^[ 	]*/  /mg;	# normalize leading whitespace
    s/\s*;\s*(#\s*)?$//;	# remove trailing semicolon(+newline-hash)

    next if $opt{g} and !m/$opt{g}/io;

    unless ($opt{a}) {
	s/\b\d+\b/N/g;
	s/\b0x[0-9A-Fa-f]+\b/N/g;
        s/''/'S'/g;
        s/""/"S"/g;
        s/(\\')//g;
        s/(\\")//g;
        s/'[^']+'/'S'/g;
        s/"[^"]+"/"S"/g;
	# -n=8: turn log_20001231 into log_NNNNNNNN
	s/([a-z_]+)(\d{$opt{n},})/$1.('N' x length($2))/ieg if $opt{n};
	# abbreviate massive "in (...)" statements and similar
	s!(([NS],){100,})!sprintf("$2,{repeated %d times}",length($1)/2)!eg;
    }

    my $s = $stmt{$_} ||= { users=>{}, hosts=>{} };
    $s->{c} += 1;
    $s->{t} += $t;
    $s->{l} += $l;
    $s->{r} += $r;
    $s->{e} += $e;
    $s->{a} += $a;
    $s->{users}->{$user}++ if $user;
    $s->{hosts}->{$host}++ if $host;

    warn "{{$_}}\n\n" if $opt{d};	# show processed statement string
}

foreach (keys %stmt) {
    my $v = $stmt{$_} || die;
    my ($c, $t, $l, $r, $e, $a) = @{ $v }{qw(c t l r e a)};
    $v->{at} = $t / $c;
    $v->{al} = $l / $c;
    $v->{ar} = $r / $c;
    $v->{ae} = $e / $c;
    $v->{aa} = $a / $c;
}

my @sorted = sort { $stmt{$b}->{$opt{s}} <=> $stmt{$a}->{$opt{s}} } keys %stmt;
@sorted = @sorted[0 .. $opt{t}-1] if $opt{t};
@sorted = reverse @sorted         if $opt{r};

if(!$opt{json}) {
    foreach (@sorted) {
        my $v = $stmt{$_} || die;
        my ($c, $t, $at, $l, $al, $r, $ar, $e, $ae, $a, $aa) = @{ $v }{qw(c t at l al r ar e ae a aa)};
        my @users = keys %{$v->{users}};
        my $user  = (@users==1) ? $users[0] : sprintf "%dusers",scalar @users;
        my @hosts = keys %{$v->{hosts}};
        my $host  = (@hosts==1) ? $hosts[0] : sprintf "%dhosts",scalar @hosts;
        printf "Count: %d  Time=%.2fs (%ds)  Lock=%.2fs (%ds)  Rows_sent=%.1f (%d), Rows_examined=%.1f (%d), Rows_affected=%.1f (%d), $user\@$host\n%s\n\n",
            $c, $at,$t, $al,$l, $ar,$r, $ae, $e, $aa, $a, $_;
    }
} else {
    my @json_output;
    foreach (@sorted) {
        my $v = $stmt{$_} || die;
        my ($c, $t, $at, $l, $al, $r, $ar, $e, $ae, $a, $aa) = @{ $v }{qw(c t at l al r ar e ae a aa)};
        my @users = keys %{$v->{users}};
        my $user  = (@users==1) ? $users[0] : sprintf "%dusers",scalar @users;
        my @hosts = keys %{$v->{hosts}};
        my $host  = (@hosts==1) ? $hosts[0] : sprintf "%dhosts",scalar @hosts;

        # parse the engine data
        my %engine;
        if ($_ =~ /^\s*#\s*Pages_accessed:\s*(\S+)\s+Pages_read:\s*(\S+)\s+Pages_prefetched:\s*(\S+)\s+Pages_updated:\s*(\S+)\s+Old_rows_read:\s*(\S+)/m) {
            @engine{qw(Pages_accessed Pages_read Pages_prefetched Pages_updated Old_rows_read)} = ($1, $2, $3, $4, $5);
        }
        if ($_ =~ /^\s*#\s*Pages_read_time:\s*(\S+)\s+Engine_time:\s*(\S+)/m) {
            @engine{qw(Pages_read_time Engine_time)} = ($1, $2);
        }
        # convert engine data to numbers
        map { $engine{$_} += 0 } keys %engine if $opt{a};

        # build a structured explain output
        my @explain_lines = ($_ =~ /^\s*# explain: (.+)$/mg);
        my $explain;
        if (@explain_lines >= 2) {
            my @headers = split /\s+/, shift @explain_lines;
            $explain = [
                map {
                    my @values = split /\s+/, $_;
                    my %row;
                    @row{@headers} = @values;
                    \%row;
                } @explain_lines
            ];
            # normalize the explain data
            foreach my $row (@$explain) {
                foreach my $key (keys %$row) {
                    my $val = $row->{$key};
                    $row->{$key} = undef     if $val eq 'NULL';
                    $row->{$key} = $val + 0  if $opt{a} and $val =~ /^\d+(?:\.\d+)?$/;
                }
            }
        }

        # get the query string
        (my $query = $_) =~ s/^\s*#.*\n//mg;
        $query =~ s/^\s+|\s+$//g; # trim leading/trailing whitespace

        # output the data as JSON
        push @json_output, {
            count           => $c,
            avg_time        => $at,
            total_time      => $t,
            avg_lock        => $al,
            total_lock      => $l,
            avg_rows_sent   => $ar,
            total_rows_sent => $r,
            avg_examined    => $ae,
            total_examined  => $e,
            avg_affected    => $aa,
            total_affected  => $a,
            user            => $user,
            host            => $host,
            query           => $query,
            engine          => (%engine ? \%engine : undef),
            explain         => ($explain ? $explain : undef),
        };
    }
    print JSON->new->canonical(1)->pretty->encode(\@json_output);
}

sub usage {
    my $str= shift;
    my $text= <<HERE;
Usage: mysqldumpslow [ OPTS... ] [ LOGS... ]

Parse and summarize the MySQL slow query log. Options are

  --verbose    verbose
  --debug      debug
  --help       write this text to standard output
  --json       print as a JSON-formatted string

  -v           verbose
  -d           debug
  -s ORDER     what to sort by (aa, ae, al, ar, at, a, c, e, l, r, t), 'at' is default
                aa: average rows affected
                ae: aggregated rows examined
                al: average lock time
                ar: average rows sent
                at: average query time
                 a: rows affected
                 c: count
                 e: rows examined 
                 l: lock time
                 r: rows sent
                 t: query time  
  -r           reverse the sort order (largest last instead of first)
  -t NUM       just show the top n queries
  -a           don't abstract all numbers to N and strings to 'S'
  -n NUM       abstract numbers with at least n digits within names
  -g PATTERN   grep: only consider stmts that include this string
  -h HOSTNAME  hostname of db server for *-slow.log filename (can be wildcard),
               default is '*', i.e. match all
  -i NAME      name of server instance (if using mysql.server startup script)
  -l           don't subtract lock time from total time
  -j           print as a JSON-formatted string

HERE
    if ($str) {
      print STDERR "ERROR: $str\n\n";
      print STDERR $text;
      exit 1;
    } else {
      print $text;
      exit 0;
    }
}
