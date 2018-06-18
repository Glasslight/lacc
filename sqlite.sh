#!/bin/sh

comp=$1
if [ -z "$comp" ] || [ ! -f "$comp" ]
then
	echo "Usage: $0 <compiler>";
	exit 1
fi

if [ ! -f sqlite/shell.c ] || [ ! -f sqlite/sqlite3.c ]
then
	echo "Missing sqlite source, download and place in 'sqlite' folder"
	exit 1
fi

flags="-DSQLITE_DEBUG -DSQLITE_MEMDEBUG -c -v --dump-symbols --dump-types"

# Build with lacc
$comp -fPIC -c sqlite/shell.c -o bin/shell.o
valgrind $comp -fPIC $flags sqlite/sqlite3.c -o bin/sqlite3.o > /dev/null
cc bin/shell.o bin/sqlite3.o -o bin/sqlite -lm -lpthread -ldl

# Build with reference compiler
cc sqlite/shell.c sqlite/sqlite3.c -o bin/sqlite-cc -lm -lpthread -ldl

# Test case
input=$(cat <<EOF
create table tbl1(one varchar(10), two smallint);
insert into tbl1 values('hello!', 10);
insert into tbl1 values('goodbye', 20);
select * from tbl1;
EOF
)

expected=$(echo "$input" | bin/sqlite-cc)
actual=$(echo "$input" | bin/sqlite)

if [ "$expected" != "$actual" ]
then
	echo "$(tput setaf 1)Wrong output!$(tput sgr0)";
	exit 1
fi

echo "$(tput setaf 2)Ok!$(tput sgr0)"
exit 0
