# run by the test. a failed check exits with its number
fail() { echo "busybox: check $1 failed"; exit "$1"; }

mkdir /tmp || fail 1
echo hello > /tmp/a || fail 2
[ "$(cat /tmp/a)" = hello ] || fail 3

echo hello | wc -c > /tmp/n
[ "$(cat /tmp/n)" = 6 ] || fail 4

cd /tmp || fail 5
[ "$(pwd)" = /tmp ] || fail 6
[ "$(cat a)" = hello ] || fail 7

cd .. && cd tmp/../tmp || fail 8
[ "$(pwd)" = /tmp ] || fail 9

ls /tmp | grep -q '^a$' || fail 10

echo more >> a
[ "$(wc -l < a)" = 2 ] || fail 11

(exit 3)
[ $? = 3 ] || fail 12

ls /nonexistent 2> /tmp/err
[ $? != 0 ] || fail 13
[ -s /tmp/err ] || fail 14

# more than a pipe holds, and yes dies of SIGPIPE when head is done
[ "$(yes | head -n 30000 | wc -l)" = 30000 ] || fail 15

sh -c 'exit 7'
[ $? = 7 ] || fail 16

ls /etc | grep -q motd || fail 17

# removing and moving: rm, mv, cp, ln, touch, rmdir
mkdir /tmp/d || fail 18
echo one > /tmp/d/f || fail 19
cp /tmp/d/f /tmp/d/g || fail 20
mv /tmp/d/g /tmp/d/h || fail 21
[ ! -e /tmp/d/g ] && [ "$(cat /tmp/d/h)" = one ] || fail 22
ln /tmp/d/h /tmp/d/hard || fail 23
ln -s h /tmp/d/soft || fail 24
[ "$(cat /tmp/d/soft)" = one ] && [ "$(readlink /tmp/d/soft)" = h ] || fail 25
touch /tmp/d/new || fail 26
[ -e /tmp/d/new ] || fail 27
rm /tmp/d/hard /tmp/d/soft /tmp/d/new || fail 28
rmdir /tmp/d 2> /dev/null && fail 29
rm -r /tmp/d || fail 30
[ ! -e /tmp/d ] || fail 31

# /dev
echo gone > /dev/null || fail 32
[ "$(head -c 4 /dev/zero | wc -c)" = 4 ] || fail 33
[ "$(head -c 16 /dev/urandom | wc -c)" = 16 ] || fail 34
[ -c /dev/tty ] && [ -c /dev/null ] || fail 35

echo "busybox: all checks passed"
exit 0
