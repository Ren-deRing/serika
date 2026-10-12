# run by the busybox test. a failed check exits with its number
fail() { echo "busybox: check $1 failed"; exit "$1"; }

busybox mkdir /tmp || fail 1
echo hello > /tmp/a || fail 2
[ "$(busybox cat /tmp/a)" = hello ] || fail 3

echo hello | busybox wc -c > /tmp/n
[ "$(busybox cat /tmp/n)" = 6 ] || fail 4

cd /tmp || fail 5
[ "$(pwd)" = /tmp ] || fail 6
[ "$(busybox cat a)" = hello ] || fail 7

cd .. && cd tmp/../tmp || fail 8
[ "$(pwd)" = /tmp ] || fail 9

busybox ls /tmp | busybox grep -q '^a$' || fail 10

echo more >> a
[ "$(busybox wc -l < a)" = 2 ] || fail 11

(exit 3)
[ $? = 3 ] || fail 12

busybox ls /nonexistent 2> /tmp/err
[ $? != 0 ] || fail 13
[ -s /tmp/err ] || fail 14

# more than a pipe holds, and yes dies of SIGPIPE when head is done
[ "$(busybox yes | busybox head -n 30000 | busybox wc -l)" = 30000 ] || fail 15

busybox sh -c 'exit 7'
[ $? = 7 ] || fail 16

busybox ls /etc | busybox grep -q motd || fail 17

echo "busybox: all checks passed"
exit 0
