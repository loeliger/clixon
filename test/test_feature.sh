#!/bin/bash
# Yang features. if-feature
APPNAME=example
# include err() and new() functions and creates $dir
. ./lib.sh

cfg=$dir/conf_yang.xml
fyang=$dir/test.yang

cat <<EOF > $cfg
<config>
  <CLICON_FEATURE>$APPNAME:A</CLICON_FEATURE>
  <CLICON_FEATURE>ietf-routing:router-id</CLICON_FEATURE>
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_YANG_DIR>/usr/local/share/$APPNAME/yang</CLICON_YANG_DIR>
  <CLICON_YANG_MODULE_MAIN>$APPNAME</CLICON_YANG_MODULE_MAIN>
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_CLI_GENMODEL_COMPLETION>1</CLICON_CLI_GENMODEL_COMPLETION>
  <CLICON_XMLDB_DIR>/usr/local/var/$APPNAME</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_PLUGIN>/usr/local/lib/xmldb/text.so</CLICON_XMLDB_PLUGIN>
  <CLICON_MODULE_LIBRARY_RFC7895>true</CLICON_MODULE_LIBRARY_RFC7895>
</config>
EOF

cat <<EOF > $fyang
module $APPNAME{
   prefix ex;
   import ietf-routing {
	prefix rt;
   }
   feature A{
      description "This test feature is enabled";
   }
   feature B{
      description "This test feature is disabled";
   }
   feature C{
      description "This test feature is disabled";
   }
   leaf x{
     if-feature A;
     type "string";
   }
   leaf y{
     if-feature B;
     type "string";
   }
   leaf z{
     type "string";
   }
}
EOF
new "start backend -s init -f $cfg -y $fyang"
# kill old backend (if any)
new "kill old backend"
sudo clixon_backend -zf $cfg -y $fyang
if [ $? -ne 0 ]; then
    err
fi

new "start backend -s init -f $cfg -y $fyang"
# start new backend
sudo $clixon_backend -s init -f $cfg -y $fyang
if [ $? -ne 0 ]; then
    err
fi

new "cli enabled feature"
expectfn "$clixon_cli -1f $cfg -y $fyang set x foo" 0 ""

new "cli disabled feature"
expectfn "$clixon_cli -1f $cfg -l o -y $fyang set y foo" 255 "CLI syntax error: \"set y foo\": Unknown command"

new "cli enabled feature in other module"
expectfn "$clixon_cli -1f $cfg -y $fyang set routing routing-instance A router-id 1.2.3.4" 0 ""

new "cli disabled feature in other module"
expectfn "$clixon_cli -1f $cfg -l o -y $fyang set routing routing-instance A default-ribs" 255 "CLI syntax error: \"set routing routing-instance A default-ribs\": Unknown command"

new "netconf discard-changes"
expecteof "$clixon_netconf -qf $cfg -y $fyang" 0 "<rpc><discard-changes/></rpc>]]>]]>" "^<rpc-reply><ok/></rpc-reply>]]>]]>$"

new "netconf enabled feature"
expecteof "$clixon_netconf -qf $cfg -y $fyang" 0 "<rpc><edit-config><target><candidate/></target><config><x>foo</x></config></edit-config></rpc>]]>]]>" "^<rpc-reply><ok/></rpc-reply>]]>]]>$"

new "netconf validate enabled feature"
expecteof "$clixon_netconf -qf $cfg -y $fyang" 0 "<rpc><validate><source><candidate/></source></validate></rpc>]]>]]>" "^<rpc-reply><ok/></rpc-reply>]]>]]>$"

new "netconf disabled feature"
expecteof "$clixon_netconf -qf $cfg -y $fyang" 0 "<rpc><edit-config><target><candidate/></target><config><A>foo</A></config></edit-config></rpc>]]>]]>" '^<rpc-reply><rpc-error><error-tag>operation-failed</error-tag><error-type>protocol</error-type><error-severity>error</error-severity><error-message>XML node config/A has no corresponding yang specification \(Invalid XML or wrong Yang spec?'

new "Kill backend"
# kill backend
sudo clixon_backend -zf $cfg
if [ $? -ne 0 ]; then
    err "kill backend"
fi

# Check if still alive
pid=`pgrep clixon_backend`
if [ -n "$pid" ]; then
    sudo kill $pid
fi

rm -rf $dir