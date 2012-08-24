use strict;
use warnings;
use blib;
use Sereal::Encoder qw(encode_sereal);
use Benchmark qw(cmpthese);
use JSON::XS qw(encode_json);
use Data::Dumper qw(Dumper);
use Data::Dumper::Limited qw(DumpLimited);
use Storable qw(nfreeze thaw);
use Data::MessagePack;
use Getopt::Long qw(GetOptions);

GetOptions(
  'dump|d' => \(my $dump),
);

our %opt = @ARGV;

use constant SEREAL_ONLY => 0;

our $mpo = Data::MessagePack->new();

my @str;
push @str, join("", map chr(65+int(rand(57))), 1..10) for 1..1000;

our %data;
$data{$_}= [
  [1..10000],
  {@str},
  {@str},
  [1..10000],
  {@str},
  [map rand,1..1000],
  {@str},
  {@str},
] for qw(sereal dd1 dd2 ddl mp json_xs storable);

my ($json_xs, $dd1, $dd2, $ddl, $sereal, $storable, $mp);
# do this first before any of the other dumpers "contaminate" the iv/pv issue
$sereal   = encode_sereal($data{sereal}, \%opt);
if (!SEREAL_ONLY) {
  $json_xs  = encode_json($data{json_xs});
  $dd1      = Data::Dumper->new([$data{dd1}])->Indent(0)->Dump();
  $dd2      = Dumper($data{dd2});
  $ddl      = DumpLimited($data{ddl});
  $mp       = $mpo->pack($data{mp});
  $storable = nfreeze($data{storable}); # must be last
}

print($sereal), exit if $dump;

require bytes;
if (!SEREAL_ONLY) {
  print "JSON::XS:              " . bytes::length($json_xs) . " bytes\n";
  print "Data::Dumper::Limited: " . bytes::length($ddl) . " bytes\n";
  print "Data::Dumper (1):      " . bytes::length($dd1) . " bytes\n";
  print "Data::Dumper (2):      " . bytes::length($dd2) . " bytes\n";
  print "Storable:              " . bytes::length($storable) . " bytes\n";
  print "Data::MessagePack:     " . bytes::length($mp) . " bytes\n";
}
print "Sereal::Encoder:       " . bytes::length($sereal) . " bytes\n";

our $x;
cmpthese(
  -3,
  {
    (!SEREAL_ONLY
      ? (
        json_xs => '$::x = encode_json($::data{json_xs});',
        ddl => '$::x = DumpLimited($::data{ddl});',
        dd1 => '$::x = Data::Dumper->new([$::data{dd1}])->Indent(0)->Dump();',
        dd2 => '$::x = Dumper($::data{dd2});',
        storable => '$::x = nfreeze($::data{storable});',
        mp => '$::x = $::mpo->pack($::data{mp});',
      ) : ()),
    sereal => '$::x = encode_sereal($::data{sereal}, \%::opt);',
  }
);

