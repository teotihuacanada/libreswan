: ==== start ====
TESTNAME=ikev2-09-rw-rsa
/testing/pluto/bin/wait-until-network-ready
source /testing/pluto/bin/westnlocal.sh

ipsec setup stop
rm -f /tmp/pluto.log
ln -s /testing/pluto/$TESTNAME/OUTPUT/pluto.west.log /tmp/pluto.log

# confirm that the network is alive
ping -n -c 2 -I 192.0.1.254 192.0.2.254

# make sure that clear text does not get through
iptables -A INPUT -i eth1 -s 192.0.2.0/24 -j DROP
# confirm with a ping to east-in
ping -n -c 1 -I 192.0.1.254 192.0.2.254

ipsec setup start
/testing/pluto/bin/wait-until-pluto-started

ipsec whack --whackrecord /var/tmp/ikev2.record
ipsec auto --add westnet-eastnet-ikev2
ipsec auto --status

echo "initdone"

