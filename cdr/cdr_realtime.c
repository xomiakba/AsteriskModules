/*!
 * \file
 * \brief Custom Realtime CDR records.
 *
 * \author Bulatov A bulatov_an@magnit.ru
 *
 *
 * \arg See also \ref AstCDR
 *
 *
 * \ingroup cdr_drivers
 */

/*** MODULEINFO
	<depend></depend>
	<support_level>extended</support_level>
	<defaultenabled>no</defaultenabled>
 ***/

#include "asterisk.h"
#include "asterisk/channel.h"
#include "asterisk/cdr.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/cli.h"
#include "asterisk/app.h"

AST_MUTEX_DEFINE_STATIC(lock);

static const char config_file[] = "cdr_realtime.conf";
static const char desc[] = "Customizable Realtime CDR Backend";
static const char name[] = "cdr_realtime";

static char RT_Engine[30];
struct ast_variable *fields = NULL;
int FilterCDR=0;

static void free_config(int reload);

static int load_config(int reload)
{
	struct ast_config *cfg;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	struct ast_variable *var, *first=NULL;
	const char *temp=NULL;
	

	if ((cfg = ast_config_load(config_file, config_flags)) == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING, "Failed to %sload configuration file. %s\n", reload ? "re" : "", reload ? "" : "Module not activated.");
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		ast_verb(3, "CDR Realtime config unchanged, skip reload options. Do Work!\n");
		return 0;
	}

	if(reload){ast_mutex_lock(&lock);}
	{ // Start load
		free_config(0);
		// Load main params
		temp=ast_variable_retrieve(cfg, "general", "engine");
		if(temp){
			ast_copy_string(RT_Engine, temp, sizeof(RT_Engine)-1);
		} else {
			ast_copy_string(RT_Engine,"CDR",sizeof(RT_Engine)-1);
			ast_verb(3, "CDR Realtime not found general/engine, use built-in engine name -> CDR\n");
		}
		ast_verb(3, "CDR Realtime [Engine] start CDR flow to engine -> %s\n",RT_Engine);
		
		FilterCDR=0;
		temp=ast_variable_retrieve(cfg, "general", "filter");
		if(temp){
			if(ast_true(temp)!=0){
				FilterCDR=1;
				ast_verb(3, "CDR Realtime enable filter CDR by CDR(amaflags) = BILLING\n");
			}
		}
	
		var=ast_variable_browse(cfg, "columns");
		if (var) {
			for (; var; var = var->next) {
				if(!fields){
					fields=ast_variable_new(var->name, var->value,"");
					first=fields;
				} else {
					first->next=ast_variable_new(var->name, var->value,"");
					first=first->next;
				}
				ast_verb(3, "CDR Realtime [add column] %s -> %s\n",var->name, var->value);
			}
		}
	}
	if(reload){ast_mutex_unlock(&lock);}

	ast_config_destroy(cfg);
	return 0;
}

static void free_config(int reload)
{
	struct ast_variable *var;
	
	if(fields){
		ast_verb(3,"Realtime CDR: free configurations fields\n");
		var=fields;
		fields=NULL;
		ast_variables_destroy(var);
	}
	FilterCDR=0;
	return;
}

static int write_cdr(struct ast_cdr *cdr)
{
	if(!fields){
		ast_log(LOG_ERROR, "CDR Realtime - no fields for write in engine, skip CDR update\n");
		//ast_cdr_backend_suspend(name);
		return 0;
	}
	
	if(!cdr){
		ast_log(LOG_WARNING, "CDR Realtime - no CDR, skip\n");
		return 0;
	}
	
	if(FilterCDR){
		ast_verb(3,"%s - billing flag is %s\n",cdr->channel,ast_channel_amaflags2string(cdr->amaflags));
		if(cdr->amaflags != AST_AMA_BILLING){
			ast_verb(3,"%s - skip write CDR in engine %s - non billing record\n",cdr->channel,RT_Engine);
			return 0;
		}
	}
	
	// Do CDR here
	{
		// Make variables
		struct ast_channel *dummy=ast_dummy_channel_alloc();
		struct ast_variable *values=NULL; // real data, need free
		struct ast_variable *var, *first=NULL; // this is only pointer to values and fields struct, no need free after
		char subst_buf[1024]="";
		
		if(dummy) {
			ast_channel_cdr_set(dummy, ast_cdr_dup(cdr));
			ast_mutex_lock(&lock);
			var=fields;
			for (; var; var = var->next) {
				pbx_substitute_variables_helper(dummy, var->value, subst_buf, sizeof(subst_buf) - 1);
				if(!values){
					values=ast_variable_new(var->name,subst_buf,"");
					first=values;
				} else {
					values->next=ast_variable_new(var->name,subst_buf,"");
					values=values->next;
				}
			}
			ast_mutex_unlock(&lock);
			ast_channel_unref(dummy);
			if(first){
				if(ast_store_realtime_fields(RT_Engine,first)<0?1:0){
					ast_log(LOG_ERROR, "%s - Error write CDR via realtime in engine %s\n",cdr->channel,RT_Engine);
				} else {
					ast_verb(3,"%s - write CDR via realtime in engine %s OK\n",cdr->channel,RT_Engine);
				}
				ast_variables_destroy(first);
			} else {
				ast_log(LOG_ERROR, "%s - No data for send to engine %s. Wat??\n",cdr->channel,RT_Engine);
			}
		} else {
			ast_log(LOG_ERROR, "Unable to allocate channel for variable subsitution.\n");
		}
	}

	return 0;
}

static int unload_module(void)
{
	if (ast_cdr_unregister(name)) {
		return -1;
	} else {
		free_config(0);
		return 0;
	}
}

static int load_module(void)
{
	if (load_config(0)) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_cdr_register(name, desc, write_cdr)) {
		ast_log(LOG_ERROR, "Unable to register custom Realtime CDR handling\n");
		free_config(0);
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	int res = 0;

	ast_mutex_lock(&lock);
	res = load_config(1);
	ast_mutex_unlock(&lock);

	return res;
}


AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Realtime CDR Module",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
	.load_pri = AST_MODPRI_CDR_DRIVER,
	.requires = "cdr",
);
