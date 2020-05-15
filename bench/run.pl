#!/usr/bin/perl -w
#
# Runs commands in test_list file for a number of runs and prints
# results to stdout:
#
#   $ ./run.pl command-list.txt 5 > results.out

if( $#ARGV < 1 ) {
    print "Usage: run.pl command-file number-of-runs\n";
    exit;
}

$test_file = $ARGV[0];
$num = $ARGV[1];

print "test_file: $test_file.\n";
print "num of runs: $num.\n";

open INPUT, "<$test_file" or die "Can't open $test_file";

while(my $cmd = <INPUT>) {
    next if $cmd =~ /^#/;

    print "START: $cmd"; 

    for($i=0; $i < $num; $i++) {
        @res = `$cmd`;
        print @res; 
        print "\n";
    }
    print STDERR "Done with $cmd";
    print "-------------------------------------------------\n\n";
}
