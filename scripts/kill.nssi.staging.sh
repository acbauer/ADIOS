#!/bin/sh

SERVER_CONTACT_INFO=$1 ; shift

SVC_LIST=`cat ${SERVER_CONTACT_INFO}.* | awk '{ ML=$0" "ML } END { print ML }'`
for SVC in $SVC_LIST; do
SVC_NID=`echo $SVC | sed -e 's/\(.*\)@\(.*\)@\(.*\)@\(.*\)/\1/'`
SVC_PID=`echo $SVC | sed -e 's/\(.*\)@\(.*\)@\(.*\)@\(.*\)/\2/'`
SVC_ADDR=`echo $SVC | sed -e 's/\(.*\)@\(.*\)@\(.*\)@\(.*\)/\3/'`
SVC_PORT=`echo $SVC | sed -e 's/\(.*\)@\(.*\)@\(.*\)@\(.*\)/\4/'`
  /ccs/home/thkorde/projects/nessie/install/x86_64/bin/nssi-kill --server-nid="$SVC_NID" --server-pid="$SVC_PID"

exit
done
