#!/bin/sh

case "$1" in
  start)
    start-stop-daemon --start --background --make-pidfile --pidfile /var/run/aesdsocket.pid --exec /usr/bin/aesdsocket -- -d
    ;;
  stop)
    start-stop-daemon --stop --pidfile /var/run/aesdsocket.pid --retry 5
    ;;
  restart)
    $0 stop
    $0 start
    ;;
  *)
    echo "Usage: $0 {start|stop|restart}"
    exit 1
    ;;
esac

exit 0
