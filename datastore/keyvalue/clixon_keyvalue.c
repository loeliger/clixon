/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
 */
/*
 * An xml database consists of key-value pairs for xml-trees.
 * Each node in an xml-tree has a key and an optional value.
 * The key (xmlkey) is constructed from the xml node name concatenated
 * with its ancestors and any eventual list keys.
 * A xmlkeyfmt is a help-structure used when accessing the XML database.
 * It consists of an xmlkey but with the key fields replaced with wild-chars(%s)
 * Example: /aaa/bbb/%s/%s/ccc
 * Such an xmlkeyfmt can be obtained from a yang-statement by following
 * its ancestors to the root module. If one of the ancestors is a list, 
 * a wildchar (%s) is inserted for each key.
 * These xmlkeyfmt keys are saved and used in cli callbacks such as when
 * modifying syntax (eg cli_merge/cli_delete) or when completing for sub-symbols
 * In this case, the variables are set and the wildcards can be instantiated.
 * An xml tree can then be formed that can be used to the xmldb_get() or 
 * xmldb_put() functions.
 * The relations between the functions and formats are as follows:
 *
 * +-----------------+                   +-----------------+
 * | yang-stmt       | yang2api_path_fmt |   api_path_fmt  | api_path_fmt2xpath
 * | list aa,leaf k  | ----------------->|     /aa=%s      |---------------->
 * +-----------------+                   +-----------------+
 *                                               |
 *                                               | api_path_fmt2api_path
 *                                               | k=17
 *                                               v
 * +-------------------+                +-----------------+
 * | xml-tree/cxobj    |   xmlkey2xml   |api_path  RFC3986|
 * | <aa><k>17</k></aa>| <------------- |   /aa=17        |
 * +-------------------+                +-----------------+
 *
 * Alternative for xmlkeyfmt would be eg:
 * RESTCONF:     /interfaces/interface=%s/ipv4/address/ip=%s (used)
 * XPATH:        /interfaces/interface[name='%s']/ipv4/address/[ip'=%s']
 *
 * Paths through the code (for coverage)
 *   cli_callback_generate       +----------------+
 *   cli_expand_var_generate     | yang2api_path_fmt |
 *  yang  ------------->         |                |
 *                               +----------------+
 * dependency on clixon handle:
 * clixon_xmldb_dir()
 * clicon_dbspec_yang(h)
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
#include <syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_chunk.h"
#include "clixon_qdb.h"
#include "clixon_keyvalue.h"

#define handle(xh) (assert(kv_handle_check(xh)==0),(struct kv_handle *)(xh))

/* Magic to ensure plugin sanity. */
#define KV_HANDLE_MAGIC 0xfa61a402

/*! Internal structure of keyvalue datastore handle. 
 */
struct kv_handle {
    int        kh_magic;    /* magic */
    char      *kh_dbdir;    /* Directory of database files */
    yang_spec *kh_yangspec;    /* Yang spec if this datastore */
};

/*! Check struct magic number for sanity checks
 * return 0 if OK, -1 if fail.
 */
static int
kv_handle_check(xmldb_handle xh)
{
    /* Dont use handle macro to avoid recursion */
    struct kv_handle *kh = (struct kv_handle *)(xh);

    return kh->kh_magic == KV_HANDLE_MAGIC ? 0 : -1;
}

/*! Database locking for candidate and running non-persistent
 * Store an integer for running and candidate containing
 * the session-id of the client holding the lock.
 */
static int _running_locked = 0;
static int _candidate_locked = 0;
static int _startup_locked = 0;

/*! Translate from symbolic database name to actual filename in file-system
 * @param[in]   xh       XMLDB handle
 * @param[in]   db       Symbolic database name, eg "candidate", "running"
 * @param[out]  filename Filename. Unallocate after use with free()
 * @retval      0        OK
 * @retval     -1        Error
 * @note Could need a way to extend which databases exists, eg to register new.
 * The currently allowed databases are: 
 *   candidate, tmp, running, result
 * The filename reside in CLICON_XMLDB_DIR option
 */
static int
kv_db2file(struct kv_handle *kh, 
	   const char       *db,
	   char            **filename)
{
    int   retval = -1;
    cbuf *cb;
    char *dir;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if ((dir = kh->kh_dbdir) == NULL){
	clicon_err(OE_XML, errno, "dbdir not set");
	goto done;
    }
    if (strcmp(db, "running") != 0 && 
	strcmp(db, "candidate") != 0 && 
	strcmp(db, "startup") != 0 && 
	strcmp(db, "tmp") != 0){
	clicon_err(OE_XML, 0, "No such database: %s", db);
	goto done;
    }
    cprintf(cb, "%s/%s_db", dir, db);
    if ((*filename = strdup4(cbuf_get(cb))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Help function to append key values from an xml list to a cbuf 
 * Example, a yang node x with keys a and b results in "x/a/b"
 */
static int
append_listkeys(cbuf      *ckey, 
		cxobj     *xt, 
		yang_stmt *ys)
{
    int        retval = -1;
    yang_stmt *ykey;
    cxobj     *xkey;
    cg_var    *cvi;
    cvec      *cvk = NULL; /* vector of index keys */
    char      *keyname;
    char      *bodyenc;
    int        i=0;

    cvk = ys->ys_cvec; /* Use Y_LIST cache, see ys_populate_list() */
    cvi = NULL;
    /* Iterate over individual keys  */
    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	keyname = cv_string_get(cvi);
	if ((xkey = xml_find(xt, keyname)) == NULL){
	    clicon_err(OE_XML, errno, "XML list node \"%s\" does not have key \"%s\" child",
		       xml_name(xt), keyname);
	    goto done;
	}
	if (uri_percent_encode(&bodyenc, "%s", xml_body(xkey)) < 0)
	    goto done;
	if (i++)
	    cprintf(ckey, ",");
	else
	    cprintf(ckey, "=");
	cprintf(ckey, "%s", bodyenc);
	free(bodyenc); 
	bodyenc = NULL;
    }
    retval = 0;
 done:
    return retval;
}

/*! Help function to create xml key values 
 * @param[in,out] x   Parent
 * @param[in]     ykey
 * @param[in]     arg
 * @param[in]     keyname  yang key name
 */
static int
create_keyvalues(cxobj     *x, 
		 yang_stmt *ykey, 
		 char      *arg, 
		 char      *keyname)
{
    int        retval = -1;
    cxobj     *xn;
    cxobj     *xb;

    /* Check if key node exists */
    if ((xn = xml_new_spec(keyname, x, ykey)) == NULL)
	goto done;
    if ((xb = xml_new("body", xn)) == NULL)
	goto done;
    xml_type_set(xb, CX_BODY);
    xml_value_set(xb, arg);
    retval = 0;
 done:
    return retval;
}


/*!
 * @param[in]   xk     xmlkey
 * @param[out]  xt     XML tree as result
 * XXX cannot handle top-level list
 */
static int
get(char      *dbname,
    yang_spec *ys,
    char      *xk,
    char      *val,
    cxobj     *xt)
{
    int         retval = -1;
    char      **vec = NULL;
    int         nvec;
    char      **valvec = NULL;
    int         nvalvec;
    int         i;
    int         j;
    char       *name;
    char       *restval;
    yang_stmt  *y;
    cxobj      *x;
    cxobj      *xc;
    cxobj      *xb;
    yang_stmt *ykey;
    cg_var    *cvi;
    cvec      *cvk = NULL; /* vector of index keys */
    char      *keyname;
    char      *arg;
    char      *argdec;
    cbuf      *cb;
    
    //    clicon_debug(1, "%s xkey:%s val:%s", __FUNCTION__, xk, val);
    x = xt;
    if (xk == NULL || *xk!='/'){ 
	clicon_err(OE_DB, 0, "Invalid key: %s", xk);
	goto done;
    }
    if ((vec = clicon_strsep(xk, "/", &nvec)) == NULL)
	goto done;
    /* Element 0 is NULL '/', 
       Element 1 is top symbol and needs to find subs in all modules:
       spec->module->syntaxnode
    */
    if (nvec < 2){
	clicon_err(OE_XML, 0, "Malformed key: %s", xk);
	goto done;
    }
    i = 1;
    while (i<nvec){
	name = vec[i]; /* E.g "x=1,2" -> name:x restval=1,2 */
	if ((restval = index(name, '=')) != NULL){
	    *restval = '\0';
	    restval++;
	}
	if (i == 1){ /* spec->module->node */
	    if ((y = yang_find_topnode(ys, name, YC_DATANODE)) == NULL){
		clicon_err(OE_UNIX, errno, "No yang node found: %s", name);
		goto done;
	    }
	}
	else
	    if ((y = yang_find_datanode((yang_node*)y, name)) == NULL){
		clicon_err(OE_UNIX, errno, "No yang node found: %s", name);
		goto done;
	    }
	switch (y->ys_keyword){
	case Y_LEAF_LIST:
	    /* 
	     * If xml element is a leaf-list, then the next element is expected to
	     * be a value
	     */
	    if (uri_percent_decode(&argdec, restval) < 0)
		goto done;
	    if ((xc = xml_find(x, name))==NULL ||
		(xb = xml_find(xc, argdec))==NULL){
		if ((xc = xml_new_spec(name, x, y)) == NULL)
		    goto done;
		/* Assume body is created at end of function */
	    }
	    free(argdec);
	    argdec = NULL;
	    break;
	case Y_LIST:
	    /* 
	     * If xml element is a list, then the next element(s) is expected to be
	     * a key value. Check if this key value is already in the xml tree,
	     * otherwise create it.
	     */
	    if ((ykey = yang_find((yang_node*)y, Y_KEY, NULL)) == NULL){
		clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
			   __FUNCTION__, y->ys_argument);
		goto done;
	    }
	    /* The value is a list of keys: <key>[ <key>]*  */
	    if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
		goto done;
	    if ((cb = cbuf_new()) == NULL){
		clicon_err(OE_XML, errno, "cbuf_new");
		goto done;
	    }
	    cvi = NULL;
	    /* Iterate over individual yang keys  */
	    cprintf(cb, "%s", name);
	    if (valvec)
		free(valvec);
	    if ((valvec = clicon_strsep(restval, ",", &nvalvec)) == NULL)
		goto done;
	    if (cvec_len(cvk)!=nvalvec){
		retval = 0;
		goto done;
	    }
	    j = 0;
	    while ((cvi = cvec_each(cvk, cvi)) != NULL){
		if (j>=nvalvec)
		    break;
		arg = valvec[j++];
		if (uri_percent_decode(arg, &argdec) < 0)
		    goto done;
		cprintf(cb, "[%s='%s']", cv_string_get(cvi), argdec);
		free(argdec);
		argdec=NULL;
	    }
	    if ((xc = xpath_first(x, cbuf_get(cb))) == NULL){
		if ((xc = xml_new_spec(name, x, y)) == NULL)
		    goto done;
		cvi = NULL;
		//		i -= cvec_len(cvk);
		/* Iterate over individual yang keys  */
		j=0;
		while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		    if (j>=nvalvec)
			break;
		    arg = valvec[j++];
		    keyname = cv_string_get(cvi);
		    if (uri_percent_decode(arg, &argdec) < 0)
			goto done;
		    if (create_keyvalues(xc,
					 ykey,
					 argdec, 
					 keyname) < 0)
			goto done;
		    free(argdec); 
		    argdec = NULL;
		} /* while */
	    }
	    if (cb){
		cbuf_free(cb);
		cb = NULL;
	    }
	    if (cvk){
		cvec_free(cvk);
		cvk = NULL;
	    }
	    break;
	case Y_LEAF:
	case Y_CONTAINER:
	default: 
	    if ((xc = xml_find(x, name))==NULL)
		if ((xc = xml_new_spec(name, x, y)) == NULL)
		    goto done;
	    break;
	} /* switch */
	x = xc;
	i++;
    }
    if (val && xml_body(x)==NULL){
	if ((x = xml_new("body", x)) == NULL)
	    goto done;
	xml_type_set(x, CX_BODY);
	xml_value_set(x, val);
    }
    if(debug>1){
	fprintf(stderr, "%s %s\n", __FUNCTION__, xk);
	clicon_xml2file(stderr, xt, 0, 1);
    }
    retval = 0;
 done:
    if (vec)
	free(vec);
    if (valvec)
	free(valvec);
    if (cvk)
	cvec_free(cvk);
    return retval;
}

/*! Connect to a datastore plugin
 * @retval  handle  Use this handle for other API calls
 * @retval  NULL    Error
 * @note You can do several connects, and have multiple connections to the same
 *       datastore
 */
xmldb_handle
kv_connect(void)
{
    struct kv_handle *kh;
    xmldb_handle      xh = NULL;
    int               size;

    size = sizeof(struct kv_handle);
    if ((kh = malloc(size)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(kh, 0, size);
    kh->kh_magic = KV_HANDLE_MAGIC;
    xh = (xmldb_handle)kh;
  done:
    return xh;
}

/*! Disconnect from a datastore plugin and deallocate handle
 * @param[in]  handle  Disconect and deallocate from this handle
 * @retval     0       OK
  */
int
kv_disconnect(xmldb_handle xh)
{
    int               retval = -1;
    struct kv_handle *kh = handle(xh);

    if (kh){
	if (kh->kh_dbdir)
	    free(kh->kh_dbdir);
	free(kh);
    }
    retval = 0;
    // done:
    return retval;
}

/*! Get value of generic plugin option. Type of value is givenby context
 * @param[in]  xh      XMLDB handle
 * @param[in]  optname Option name
 * @param[out] value   Pointer to Value of option
 * @retval     0       OK
 * @retval    -1       Error
 */
int
kv_getopt(xmldb_handle xh, 
	  char        *optname,
	  void       **value)
{
    int               retval = -1;
    struct kv_handle *kh = handle(xh);

    if (strcmp(optname, "yangspec") == 0)
	*value = kh->kh_yangspec;
    else if (strcmp(optname, "dbdir") == 0)
	*value = kh->kh_dbdir;
    else{
	clicon_err(OE_PLUGIN, 0, "Option %s not implemented by plugin", optname);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Set value of generic plugin option. Type of value is givenby context
 * @param[in]  xh      XMLDB handle
 * @param[in]  optname Option name
 * @param[in]  value   Value of option
 * @retval     0       OK
 * @retval    -1       Error
 */
int
kv_setopt(xmldb_handle xh, 
	  char        *optname,
	  void        *value)
{
    int               retval = -1;
    struct kv_handle *kh = handle(xh);

    if (strcmp(optname, "yangspec") == 0)
	kh->kh_yangspec = (yang_spec*)value;
    else if (strcmp(optname, "dbdir") == 0){
	if (value && (kh->kh_dbdir = strdup((char*)value)) == NULL){
	    clicon_err(OE_UNIX, 0, "strdup");
	    goto done;
	}
    }
    else{
	clicon_err(OE_PLUGIN, 0, "Option %s not implemented by plugin", optname);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Get content of database using xpath. return a set of matching sub-trees
 * The function returns a minimal tree that includes all sub-trees that match
 * xpath.
 * This is a clixon datastore plugin of the the xmldb api
 * @see xmldb_get
 */
int
kv_get(xmldb_handle  xh,
       const char   *db, 
       char         *xpath,
       int           config,
       cxobj       **xtop)
{
    int             retval = -1;
    struct kv_handle *kh = handle(xh);
    yang_spec      *yspec;
    char           *dbfile = NULL;
    cxobj         **xvec = NULL;
    size_t          xlen;
    int             i;
    int             npairs;
    struct db_pair *pairs;
    cxobj          *xt = NULL;

    clicon_debug(2, "%s", __FUNCTION__);
    if (kv_db2file(kh, db, &dbfile) < 0)
	goto done;
    if (dbfile==NULL){
	clicon_err(OE_XML, 0, "dbfile NULL");
	goto done;
    }
    if ((yspec =  kh->kh_yangspec) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    /* Read in complete database (this can be optimized) */
    if ((npairs = db_regexp(dbfile, "", __FUNCTION__, &pairs, 0)) < 0)
	goto done;
    if ((xt = xml_new_spec("config", NULL, yspec)) == NULL)
	goto done;
    /* Translate to complete xml tree */
    for (i = 0; i < npairs; i++) {
	if (get(dbfile, 
		yspec, 
		pairs[i].dp_key, /* xml key */
		pairs[i].dp_val, /* may be NULL */
		xt) < 0)
	    goto done;
    }
    if (xpath_vec(xt, xpath?xpath:"/", &xvec, &xlen) < 0)
	goto done;
    /* If vectors are specified then filter out everything else,
     * otherwise return complete tree.
     */
    if (xvec != NULL){
	for (i=0; i<xlen; i++)
	    xml_flag_set(xvec[i], XML_FLAG_MARK);
    }
    /* Top is special case */
    if (!xml_flag(xt, XML_FLAG_MARK))
	if (xml_tree_prune_flagged_sub(xt, XML_FLAG_MARK, 1, NULL) < 0)
	    goto done;
    if (xml_apply(xt, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)XML_FLAG_MARK) < 0)
	goto done;
    /* Add default values (if not set) */
    if (xml_apply(xt, CX_ELMNT, xml_default, NULL) < 0)
	goto done;
    /* Order XML children according to YANG */
    if (xml_apply(xt, CX_ELMNT, xml_order, NULL) < 0)
	goto done;
    if (xml_apply(xt, CX_ELMNT, xml_sanity, NULL) < 0)
	goto done;
    if (debug>1)
    	clicon_xml2file(stderr, xt, 0, 1);
    *xtop = xt;
    retval = 0;
 done:
    if (dbfile)
	free(dbfile);
    if (xvec)
	free(xvec);
    unchunk_group(__FUNCTION__);  
    return retval;

}

/*! Add data to database internal recursive function
 * @param[in]  dbfile Name of database to search in (filename incl dir path)
 * @param[in]  xt     xml-node.
 * @param[in]  ys     Yang statement corresponding to xml-node
 * @param[in]  op     OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @param[in]  xkey0  aggregated xmlkey
 * @retval     0      OK
 * @retval     -1     Error
 * @note XXX op only supports merge
 */
static int
put(char               *dbfile, 
    cxobj              *xt,
    yang_stmt          *ys, 
    enum operation_type op, 
    const char         *xk0)
{
    int        retval = -1;
    cxobj     *x = NULL;
    char      *xk;
    cbuf      *cbxk = NULL;
    char      *body;
    yang_stmt *y;
    int        exists;
    char      *bodyenc=NULL;
    char      *opstr;

    clicon_debug(1, "%s xk0:%s ys:%s", __FUNCTION__, xk0, ys->ys_argument);
    if (debug){
	xml_print(stderr, xt);
	//	yang_print(stderr, (yang_node*)ys);
    }
    if ((opstr = xml_find_value(xt, "operation")) != NULL)
	if (xml_operation(opstr, &op) < 0)
	    goto done;
    body = xml_body(xt);
    if ((cbxk = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cbxk, "%s/%s", xk0, xml_name(xt));
    switch (ys->ys_keyword){
    case Y_LIST: /* Note: can have many keys */
	if (append_listkeys(cbxk, xt, ys) < 0)
	    goto done;
	break;
    case Y_LEAF_LIST:
	if (uri_percent_encode(&bodyenc, "%s", body) < 0)
	    goto done;
	cprintf(cbxk, "=%s", bodyenc);
	break;
    default:
	break;
    }
    xk = cbuf_get(cbxk);
    //    fprintf(stderr, "%s %s\n", key, body?body:"");
    /* Write to database, key and a vector of variables */
    switch (op){
    case OP_CREATE:
	if ((exists = db_exists(dbfile, xk)) < 0)
	    goto done;
	if (exists == 1){
	    clicon_err(OE_DB, 0, "OP_CREATE: %s already exists in database", xk);
	    goto done;
	}
    case OP_MERGE:
    case OP_REPLACE:
	if (db_set(dbfile, xk, body?body:NULL, body?strlen(body)+1:0) < 0)
	    goto done;
	break;
    case OP_DELETE:
	if ((exists = db_exists(dbfile, xk)) < 0)
	    goto done;
	if (exists == 0){
	    clicon_err(OE_DB, 0, "OP_DELETE: %s does not exists in database", xk);
	    goto done;
	}
    case OP_REMOVE:
	switch (ys->ys_keyword){
	case Y_LIST:
	case Y_CONTAINER:{
	    struct db_pair *pairs;
	    int             npairs;
	    cbuf           *cbrx;
	    int             i;

	    if ((cbrx = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cbrx, "^%s.*$", xk);
	    if ((npairs = db_regexp(dbfile, cbuf_get(cbrx), __FUNCTION__, 
				    &pairs, 0)) < 0)
		goto done;
	    /* Translate to complete xml tree */
	    for (i = 0; i < npairs; i++) 
		if (db_del(dbfile, pairs[i].dp_key) < 0)
		    goto done;
	    if (cbrx)
		cbuf_free(cbrx);
	    /* Skip recursion, we have deleted whole subtree */
	    retval = 0;
	    goto done;
	    break;
	}
	default:
	    if (db_del(dbfile, xk) < 0)
		goto done;
	    break;
	}

	break;
    case OP_NONE:
	break;
    }
    /* For every node, create a key with values */
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL){
	if ((y = yang_find_datanode((yang_node*)ys, xml_name(x))) == NULL){
	    clicon_err(OE_UNIX, 0, "No yang node found: %s", xml_name(x));
	    goto done;
	}
	if (put(dbfile, x, y, op, xk) < 0)
	    goto done;
    }
    retval = 0;
 done:
    if (cbxk)
	cbuf_free(cbxk);
    if (bodyenc)
	free(bodyenc);
    unchunk_group(__FUNCTION__);
    return retval;
}

/*! Modify database provided an xml tree and an operation
 * This is a clixon datastore plugin of the the xmldb api
 * @see xmldb_put
 */
int
kv_put(xmldb_handle        xh,
       const char         *db, 
       enum operation_type op,
       cxobj              *xt)
{
    int        retval = -1;
    struct kv_handle *kh = handle(xh);
    cxobj     *x = NULL;
    yang_stmt *ys;
    yang_spec *yspec;
    char      *dbfilename = NULL;

    if ((yspec =  kh->kh_yangspec) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if (kv_db2file(kh, db, &dbfilename) < 0)
	goto done;
    if (op == OP_REPLACE){
	if (db_delete(dbfilename) < 0) 
	    goto done;
	if (db_init(dbfilename) < 0) 
	    goto done;
    }
    //	clicon_log(LOG_WARNING, "%s", __FUNCTION__);
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL){
	if ((ys = yang_find_topnode(yspec, xml_name(x), YC_DATANODE)) == NULL){
	    clicon_err(OE_UNIX, errno, "No yang node found: %s", xml_name(x));
	    goto done;
	}
	if (put(dbfilename, /* database name */
		x,          /* xml root node */
		ys,         /* yang statement of xml node */
		op,         /* operation, eg merge/delete */
		""          /* aggregate xml key */
		) < 0)
	    goto done;
    }
    retval = 0;
 done:
    if (dbfilename)
	free(dbfilename);
    return retval;
}

/*! Copy database from db1 to db2
 * @param[in]  xh    XMLDB handle
 * @param[in]  from  Source database copy
 * @param[in]  to    Destination database
 * @retval -1  Error
 * @retval  0  OK
  */
int 
kv_copy(xmldb_handle xh, 
	const char  *from,
	const char  *to)
{
    int           retval = -1;
    struct kv_handle *kh = handle(xh);
    char         *fromfile = NULL;
    char         *tofile = NULL;

    /* XXX lock */
    if (kv_db2file(kh, from, &fromfile) < 0)
	goto done;
    if (kv_db2file(kh, to, &tofile) < 0)
	goto done;
    if (clicon_file_copy(fromfile, tofile) < 0)
	goto done;
    retval = 0;
 done:
    if (fromfile)
	free(fromfile);
    if (tofile)
	free(tofile);
    return retval;
}

/*! Lock database
 * @param[in]  xh      XMLDB handle
 * @param[in]  db   Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
  */
int 
kv_lock(xmldb_handle xh, 
	const char  *db,
	int          pid)
{
    int retval = -1;
    //    struct kv_handle *kh = handle(xh);
    if (strcmp("running", db) == 0)
	_running_locked = pid;
    else if (strcmp("candidate", db) == 0)
	_candidate_locked = pid;
    else if (strcmp("startup", db) == 0)
	_startup_locked = pid;
    else{
	clicon_err(OE_DB, 0, "No such database: %s", db);
	goto done;
    }
    clicon_debug(1, "%s: locked by %u",  db, pid);
    retval = 0;
 done:
    return retval;
}

/*! Unlock database
 * @param[in]  xh      XMLDB handle
 * @param[in]  db  Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
 * Assume all sanity checks have been made
 */
int 
kv_unlock(xmldb_handle xh, 
	  const char  *db)
{
    int retval = -1;
    //    struct kv_handle *kh = handle(xh);
    if (strcmp("running", db) == 0)
	_running_locked = 0;
    else if (strcmp("candidate", db) == 0)
	_candidate_locked = 0;
    else if (strcmp("startup", db) == 0)
	_startup_locked = 0;
    else{
	clicon_err(OE_DB, 0, "No such database: %s", db);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Unlock all databases locked by pid (eg process dies) 
 * @param[in]  xh    XMLDB handle
 * @param[in]  pid   Process / Session id
 * @retval    -1     Error
 * @retval     0     Ok
 */
int 
kv_unlock_all(xmldb_handle xh, 
	      int           pid)
{
    //    struct kv_handle *kh = handle(xh);

    if (_running_locked == pid)
	_running_locked = 0;
    if (_candidate_locked == pid)
	_candidate_locked = 0;
    if (_startup_locked == pid)
	_startup_locked = 0;
    return 0;
}

/*! Check if database is locked
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @retval    -1   Error
 * @retval     0   Not locked
 * @retval    >0   Id of locker
  */
int 
kv_islocked(xmldb_handle xh, 
	    const char  *db)
{
    int retval = -1;
    //    struct kv_handle *kh = handle(xh);

    if (strcmp("running", db) == 0)
	retval = _running_locked;
    else if (strcmp("candidate", db) == 0)
	retval = _candidate_locked;
    else if (strcmp("startup", db) == 0)
	retval = _startup_locked;
    else
	clicon_err(OE_DB, 0, "No such database: %s", db);
    return retval;
}

/*! Check if db exists 
 * @param[in]  xh      XMLDB handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  No it does not exist
 * @retval  1  Yes it exists
 */
int 
kv_exists(xmldb_handle xh, 
	  const char  *db)
{
    int           retval = -1;
    struct kv_handle *kh = handle(xh);
    char         *filename = NULL;
    struct stat  sb;

    if (kv_db2file(kh, db, &filename) < 0)
	goto done;
    if (lstat(filename, &sb) < 0)
	retval = 0;
    else
	retval = 1;
 done:
    if (filename)
	free(filename);
    return retval;
}

/*! Delete database. Remove file 
 * @param[in]  xh      XMLDB handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  OK
 */
int 
kv_delete(xmldb_handle xh, 
	  const char  *db)
{
    int           retval = -1;
    struct kv_handle *kh = handle(xh);
    char         *filename = NULL;

    if (kv_db2file(kh, db, &filename) < 0)
	goto done;
    if (db_delete(filename) < 0)
	goto done;
    retval = 0;
 done:
    if (filename)
	free(filename);
    return retval;
}

/*! Create / Initialize database 
 * @param[in]  xh      XMLDB handle
 * @param[in]  db  Database
 * @retval  0  OK
 * @retval -1  Error
 */
int 
kv_create(xmldb_handle xh, 
	  const char  *db)
{
    int           retval = -1;
    struct kv_handle *kh = handle(xh);
    char         *filename = NULL;

    if (kv_db2file(kh, db, &filename) < 0)
	goto done;
    if (db_init(filename) < 0)
	goto done;
    retval = 0;
 done:
    if (filename)
	free(filename);
    return retval;
}

/*! plugin init function */
int
kv_plugin_exit(void)
{
    return 0;
}

static const struct xmldb_api api;

/*! plugin init function */
void *
clixon_xmldb_plugin_init(int version)
{
    if (version != XMLDB_API_VERSION){
	clicon_err(OE_DB, 0, "Invalid version %d expected %d", 
		   version, XMLDB_API_VERSION);
	goto done;
    }
    return (void*)&api;
 done:
    return NULL;
}

static const struct xmldb_api api = {
    1,
    XMLDB_API_MAGIC,
    clixon_xmldb_plugin_init,
    kv_plugin_exit,
    kv_connect,
    kv_disconnect,
    kv_getopt,
    kv_setopt,
    kv_get,
    kv_put,
    kv_copy,
    kv_lock,
    kv_unlock,
    kv_unlock_all,
    kv_islocked,
    kv_exists,
    kv_delete,
    kv_create,
};


#if 0 /* Test program */
/*
 * Turn this on to get an xpath test program 
 * Usage: clicon_xpath [<xpath>] 
 * read xml from input
 * Example compile:
 gcc -g -o keyvalue -I. -I../../lib ./clixon_keyvalue.c clixon_chunk.c clixon_qdb.c -lclixon -lcligen -lqdbm
*/

/*! Raw dump of database, just keys and values, no xml interpretation 
 * @param[in]  f       File
 * @param[in]  dbfile  File-name of database. This is a local file
 * @param[in]  rxkey   Key regexp, eg "^.*$"
 * @note This function can only be called locally.
 */
int
main(int    argc, 
     char **argv)
{
    int             retval = -1;
    int             npairs;
    struct db_pair *pairs;
    char           *rxkey = NULL;
    char           *dbfilename;

    if (argc != 2 && argc != 3){
	fprintf(stderr, "usage: %s <dbfile> [rxkey]\n", argv[0]);
	goto done;
    }
    dbfilename = argv[1];
    if (argc == 3)
	rxkey = argv[2];
    else
	rxkey = "^.*$";     /* Default is match all */

    /* Get all keys/values for vector */
    if ((npairs = db_regexp(dbfilename, rxkey, __FUNCTION__, &pairs, 0)) < 0)
	goto done;
    
    for (npairs--; npairs >= 0; npairs--) 
	fprintf(stdout, "%s %s\n", pairs[npairs].dp_key,
		pairs[npairs].dp_val?pairs[npairs].dp_val:"");
    retval = 0;
 done:
    unchunk_group(__FUNCTION__);
    return retval;
}

#endif /* Test program */
