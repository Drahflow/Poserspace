#!/usr/bin/perl

use strict;
use warnings;

print <<"EOH";
POST /display HTTP/1.0\r
Content-type: x-poserspace/text\r
\r
EOH

$| = 1;

while(<>) {
  print;
}
