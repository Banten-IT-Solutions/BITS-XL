#define BITSXL_TEST 1
#define main bitsxl_program_main
#include "../../src/bitsxl.c"
#undef main

#include <assert.h>

static Account test_account(const char *subscription_type){
	Account account;
	memset(&account,0,sizeof(account));
	account.subscription_type=(char *)subscription_type;
	return account;
}

static void reset_decoy_config(void){
	memset(cfg.decoy_prepaid_family_code,0,sizeof(cfg.decoy_prepaid_family_code));
	memset(cfg.decoy_prepaid_package_number,0,sizeof(cfg.decoy_prepaid_package_number));
	memset(cfg.decoy_prioritas_family_code,0,sizeof(cfg.decoy_prioritas_family_code));
	memset(cfg.decoy_prioritas_package_number,0,sizeof(cfg.decoy_prioritas_package_number));
	memset(cfg.decoy_priohybrid_family_code,0,sizeof(cfg.decoy_priohybrid_family_code));
	memset(cfg.decoy_priohybrid_package_number,0,sizeof(cfg.decoy_priohybrid_package_number));
}

enum FixtureMode {
	FIXTURE_NONE,
	FIXTURE_CONTEXT,
	FIXTURE_PAYMENT,
	FIXTURE_RETRY_151,
	FIXTURE_BUILTIN_PRIORITAS,
	FIXTURE_ACCOUNT_ISOLATION,
	FIXTURE_INVALID_PACKAGE,
	FIXTURE_CANONICAL_OPTION,
	FIXTURE_DETAIL_MISMATCH,
	FIXTURE_RETRY_AUTH
};

static enum FixtureMode fixture_mode;
static int fixture_family_calls,fixture_detail_calls,fixture_refresh_calls,fixture_settlement_calls;
static int fixture_old_family_calls,fixture_new_family_calls,fixture_account_a_family_calls,fixture_account_b_family_calls;
static int fixture_expected_method_enterprise,fixture_expected_settlement_enterprise,fixture_settlement_failure;
static int fixture_retry_151_fail_after_refresh;
static int fixture_detail_mismatch_kind;
static const char *fixture_expected_payment_target,*fixture_expected_payment_for,*fixture_expected_migration;

static int payload_has(const char *payload,const char *fragment){ return payload&&strstr(payload,fragment)!=NULL; }

static char *fixture_family_response(const char *option_code,const char *variant_code,int two_options){
	if(two_options) return xasprintf(
		"{\"status\":\"SUCCESS\",\"data\":{\"package_family\":{\"name\":\"Fixture Family\"},\"package_variants\":["
		"{\"package_variant_code\":\"variant-A\",\"package_options\":[{\"order\":1,\"package_option_code\":\"option-A\"}]},"
		"{\"package_variant_code\":\"%s\",\"package_options\":[{\"order\":1,\"package_option_code\":\"%s\"}]}]}}",
		variant_code,option_code);
	return xasprintf("{\"status\":\"SUCCESS\",\"data\":{\"package_family\":{\"name\":\"Fixture Family\"},\"package_variants\":[{\"package_variant_code\":\"%s\",\"package_options\":[{\"order\":1,\"package_option_code\":\"%s\"}]}]}}",variant_code,option_code);
}

static char *fixture_detail_response(const char *option_code,const char *family_code,const char *variant_code,const char *token_confirmation){
	return xasprintf("{\"status\":\"SUCCESS\",\"data\":{\"package_option\":{\"name\":\"%s\",\"package_option_code\":\"%s\",\"price\":1000,\"validity\":\"1 day\"},\"package_family\":{\"name\":\"Fixture Family\",\"package_family_code\":\"%s\",\"payment_for\":\"BUY_PACKAGE\"},\"package_detail_variant\":{\"name\":\"%s\",\"package_variant_code\":\"%s\"},\"token_confirmation\":\"%s\",\"timestamp\":123456}}",option_code,option_code,family_code?family_code:"",variant_code,variant_code,token_confirmation);
}

static void assert_error_context(const char *error,const char *family,int package_number,const char *variant,int is_enterprise,const char *migration_type){
	char *family_fragment=xasprintf("family=%s",family),*number_fragment=xasprintf("nomor_paket=%d",package_number),*variant_fragment=xasprintf("variant=%s",variant),*enterprise_fragment=xasprintf("is_enterprise=%s",is_enterprise?"true":"false"),*migration_fragment=xasprintf("migration_type=%s",migration_type);
	assert(error&&payload_has(error,family_fragment));
	assert(payload_has(error,number_fragment));
	assert(payload_has(error,variant_fragment));
	assert(payload_has(error,enterprise_fragment));
	assert(payload_has(error,migration_fragment));
	free(family_fragment); free(number_fragment); free(variant_fragment); free(enterprise_fragment); free(migration_fragment);
}

static char *fixture_api_request(const char *path,const char *payload,const char *id_token){
	if(fixture_mode==FIXTURE_CONTEXT){
		if(!strcmp(path,"api/v8/xl-stores/options/list")){
			fixture_family_calls++;
			assert(payload_has(payload,"\"package_family_code\":\"enterprise-family\""));
			if(payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\"")&&payload_has(payload,"\"is_enterprise\":true")) return fixture_family_response("option-B","variant-B",1);
			return xstrdup("{\"status\":\"FAILED\",\"code\":\"404\"}");
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			fixture_detail_calls++;
			assert(payload_has(payload,"\"package_option_code\":\"option-B\""));
			assert(payload_has(payload,"\"package_family_code\":\"enterprise-family\""));
			assert(payload_has(payload,"\"package_variant_code\":\"variant-B\""));
			assert(payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\""));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			return fixture_detail_response("option-B","enterprise-family","variant-B","confirmation-B");
		}
		if(!strcmp(path,"misc/api/v8/utility/intercept-page")){
			assert(payload_has(payload,"\"package_option_code\":\"option-B\""));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			return xstrdup("{\"status\":\"SUCCESS\"}");
		}
		if(!strcmp(path,"payments/api/v8/payment-methods-option")){
			assert(payload_has(payload,"\"payment_target\":\"option-B\""));
			assert(payload_has(payload,"\"token_confirmation\":\"confirmation-B\""));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			return xstrdup("{\"status\":\"SUCCESS\",\"data\":{\"token_payment\":\"payment-token\",\"timestamp\":123456}}");
		}
	}
	if(fixture_mode==FIXTURE_PAYMENT){
		if(!strcmp(path,"misc/api/v8/utility/intercept-page")){
			assert(payload_has(payload,"\"package_option_code\":\"target-option\""));
			assert(payload_has(payload,"\"is_enterprise\":false"));
			return xstrdup("{\"status\":\"SUCCESS\"}");
		}
		if(!strcmp(path,"payments/api/v8/payment-methods-option")){
			char expected_enterprise[64]; snprintf(expected_enterprise,sizeof(expected_enterprise),"\"is_enterprise\":%s",fixture_expected_method_enterprise?"true":"false");
			assert(payload_has(payload,expected_enterprise));
			assert(fixture_expected_payment_target&&payload_has(payload,fixture_expected_payment_target));
			return xstrdup("{\"status\":\"SUCCESS\",\"data\":{\"token_payment\":\"payment-token\",\"timestamp\":123456}}");
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			assert(payload_has(payload,"\"migration_type\":\"NONE\""));
			assert(payload_has(payload,"\"package_family_code\":\"\""));
			assert(payload_has(payload,"\"package_variant_code\":\"\""));
			assert(payload_has(payload,"\"is_enterprise\":false"));
			return fixture_detail_response("target-option","","Target Variant","target-confirmation");
		}
	}
	if(fixture_mode==FIXTURE_RETRY_151){
		if(!strcmp(path,"api/v8/xl-stores/options/list")){
			fixture_family_calls++;
			assert(payload_has(payload,"\"package_family_code\":\"retry-family\""));
			if(!strcmp(id_token,"old-token")) fixture_old_family_calls++;
			if(!strcmp(id_token,"new-token")) fixture_new_family_calls++;
			if(!strcmp(id_token,"old-token")&&payload_has(payload,"\"migration_type\":\"PRIOH_TO_PRIO\"")&&payload_has(payload,"\"is_enterprise\":true")) return fixture_family_response("old-option","old-variant",0);
			if(!strcmp(id_token,"new-token")&&payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\"")&&payload_has(payload,"\"is_enterprise\":false")) return fixture_family_response("new-option","new-variant",0);
			return xstrdup("{\"status\":\"FAILED\",\"code\":\"404\"}");
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			fixture_detail_calls++;
			if(!strcmp(id_token,"old-token")){
				assert(payload_has(payload,"\"package_option_code\":\"old-option\""));
				assert(payload_has(payload,"\"package_variant_code\":\"old-variant\""));
				assert(payload_has(payload,"\"migration_type\":\"PRIOH_TO_PRIO\""));
				assert(payload_has(payload,"\"is_enterprise\":true"));
				return xstrdup("{\"status\":\"FAILED\",\"code\":151,\"message\":\"GENERAL_ERROR_HANDLER\",\"data\":null}");
			}
			assert(!strcmp(id_token,"new-token"));
			assert(payload_has(payload,"\"package_option_code\":\"new-option\""));
			assert(payload_has(payload,"\"package_variant_code\":\"new-variant\""));
			assert(payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\""));
			assert(payload_has(payload,"\"is_enterprise\":false"));
			if(fixture_retry_151_fail_after_refresh) return xstrdup("{\"status\":\"FAILED\",\"code\":151,\"message\":\"GENERAL_ERROR_HANDLER\",\"data\":null}");
			return fixture_detail_response("new-option","retry-family","new-variant","new-confirmation");
		}
	}
	if(fixture_mode==FIXTURE_BUILTIN_PRIORITAS){
		if(!strcmp(path,"api/v8/xl-stores/options/list")){
			fixture_family_calls++;
			assert(id_token&&!strcmp(id_token,"builtin-prio-token"));
			assert(payload_has(payload,"\"package_family_code\":\"2512b72a-a3cd-4c70-a736-132cf2c1f0c0\""));
			if(payload_has(payload,"\"migration_type\":\"PRIO_TO_PRIOH\"")&&payload_has(payload,"\"is_enterprise\":true")) return fixture_family_response("builtin-prio-option","cff298bd-8ec8-4696-b689-12407d36be15",0);
			return xstrdup("{\"status\":\"FAILED\",\"code\":\"404\"}");
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			fixture_detail_calls++;
			assert(payload_has(payload,"\"package_option_code\":\"builtin-prio-option\""));
			assert(payload_has(payload,"\"package_family_code\":\"2512b72a-a3cd-4c70-a736-132cf2c1f0c0\""));
			assert(payload_has(payload,"\"package_variant_code\":\"cff298bd-8ec8-4696-b689-12407d36be15\""));
			assert(payload_has(payload,"\"migration_type\":\"PRIO_TO_PRIOH\""));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			return fixture_detail_response("builtin-prio-option","2512b72a-a3cd-4c70-a736-132cf2c1f0c0","cff298bd-8ec8-4696-b689-12407d36be15","builtin-prio-confirmation");
		}
	}
	if(fixture_mode==FIXTURE_ACCOUNT_ISOLATION){
		if(!strcmp(path,"api/v8/xl-stores/options/list")){
			fixture_family_calls++;
			assert(payload_has(payload,"\"package_family_code\":\"shared-family\""));
			if(id_token&&!strcmp(id_token,"subscriber-a-token")){
				fixture_account_a_family_calls++;
				return fixture_family_response("subscriber-A-option","subscriber-A-variant",0);
			}
			assert(id_token&&!strcmp(id_token,"subscriber-b-token"));
			fixture_account_b_family_calls++;
			return fixture_family_response("subscriber-B-option","subscriber-B-variant",0);
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			fixture_detail_calls++;
			assert(payload_has(payload,"\"package_family_code\":\"shared-family\""));
			if(id_token&&!strcmp(id_token,"subscriber-a-token")){
				assert(payload_has(payload,"\"package_option_code\":\"subscriber-A-option\""));
				assert(payload_has(payload,"\"package_variant_code\":\"subscriber-A-variant\""));
				return fixture_detail_response("subscriber-A-option","shared-family","subscriber-A-variant","subscriber-A-confirmation");
			}
			assert(id_token&&!strcmp(id_token,"subscriber-b-token"));
			assert(payload_has(payload,"\"package_option_code\":\"subscriber-B-option\""));
			assert(payload_has(payload,"\"package_variant_code\":\"subscriber-B-variant\""));
			return fixture_detail_response("subscriber-B-option","shared-family","subscriber-B-variant","subscriber-B-confirmation");
		}
	}
	if(fixture_mode==FIXTURE_INVALID_PACKAGE){
		if(!strcmp(path,"api/v8/xl-stores/options/list")){
			fixture_family_calls++;
			assert(payload_has(payload,"\"package_family_code\":\"invalid-package-family\""));
			return fixture_family_response("only-option","only-variant",0);
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			fixture_detail_calls++;
			assert(0&&"invalid package number must stop before package detail");
		}
	}
	if(fixture_mode==FIXTURE_CANONICAL_OPTION){
		if(!strcmp(path,"api/v8/xl-stores/options/list")){
			fixture_family_calls++;
			assert(payload_has(payload,"\"package_family_code\":\"canonical-family\""));
			if(payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\"")&&payload_has(payload,"\"is_enterprise\":true")) return fixture_family_response("family-lookup-option","canonical-variant",0);
			return xstrdup("{\"status\":\"FAILED\",\"code\":\"404\"}");
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			fixture_detail_calls++;
			assert(payload_has(payload,"\"package_option_code\":\"family-lookup-option\""));
			assert(payload_has(payload,"\"package_family_code\":\"canonical-family\""));
			assert(payload_has(payload,"\"package_variant_code\":\"canonical-variant\""));
			assert(payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\""));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			return fixture_detail_response("canonical-detail-option","canonical-family","canonical-variant","canonical-confirmation");
		}
		if(!strcmp(path,"misc/api/v8/utility/intercept-page")){
			assert(payload_has(payload,"\"package_option_code\":\"canonical-detail-option\""));
			assert(!payload_has(payload,"family-lookup-option"));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			return xstrdup("{\"status\":\"SUCCESS\"}");
		}
		if(!strcmp(path,"payments/api/v8/payment-methods-option")){
			assert(payload_has(payload,"\"payment_target\":\"canonical-detail-option\""));
			assert(!payload_has(payload,"family-lookup-option"));
			assert(payload_has(payload,"\"token_confirmation\":\"canonical-confirmation\""));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			return xstrdup("{\"status\":\"SUCCESS\",\"data\":{\"token_payment\":\"canonical-payment-token\",\"timestamp\":123456}}");
		}
	}
	if(fixture_mode==FIXTURE_DETAIL_MISMATCH){
		if(!strcmp(path,"api/v8/xl-stores/options/list")){
			fixture_family_calls++;
			assert(payload_has(payload,"\"package_family_code\":\"mismatch-family\""));
			if(payload_has(payload,"\"migration_type\":\"PRIOH_TO_PRIO\"")&&payload_has(payload,"\"is_enterprise\":true")) return fixture_family_response("mismatch-option","mismatch-variant",0);
			return xstrdup("{\"status\":\"FAILED\",\"code\":\"404\"}");
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			fixture_detail_calls++;
			assert(payload_has(payload,"\"package_option_code\":\"mismatch-option\""));
			assert(payload_has(payload,"\"package_family_code\":\"mismatch-family\""));
			assert(payload_has(payload,"\"package_variant_code\":\"mismatch-variant\""));
			assert(payload_has(payload,"\"migration_type\":\"PRIOH_TO_PRIO\""));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			if(fixture_detail_mismatch_kind==1) return fixture_detail_response("canonical-mismatch-option","wrong-family","mismatch-variant","sensitive-confirmation-token");
			assert(fixture_detail_mismatch_kind==2);
			return fixture_detail_response("canonical-mismatch-option","mismatch-family","wrong-variant","sensitive-confirmation-token");
		}
	}
	if(fixture_mode==FIXTURE_RETRY_AUTH){
		if(!strcmp(path,"api/v8/xl-stores/options/list")){
			fixture_family_calls++;
			assert(payload_has(payload,"\"package_family_code\":\"auth-family\""));
			if(id_token&&!strcmp(id_token,"old-token")){
				fixture_old_family_calls++;
				return xstrdup("{\"status\":\"FAILED\",\"code\":\"132\",\"message\":\"Invalid token\",\"data\":null}");
			}
			assert(id_token&&!strcmp(id_token,"new-token"));
			fixture_new_family_calls++;
			if(payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\"")&&payload_has(payload,"\"is_enterprise\":true")) return fixture_family_response("auth-option","auth-variant",0);
			return xstrdup("{\"status\":\"FAILED\",\"code\":\"404\"}");
		}
		if(!strcmp(path,"api/v8/xl-stores/options/detail")){
			fixture_detail_calls++;
			assert(id_token&&!strcmp(id_token,"new-token"));
			assert(payload_has(payload,"\"package_option_code\":\"auth-option\""));
			assert(payload_has(payload,"\"package_family_code\":\"auth-family\""));
			assert(payload_has(payload,"\"package_variant_code\":\"auth-variant\""));
			assert(payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\""));
			assert(payload_has(payload,"\"is_enterprise\":true"));
			return fixture_detail_response("auth-option","auth-family","auth-variant","auth-confirmation");
		}
	}
	assert(0&&"unexpected fixture API request");
	return xstrdup("{\"status\":\"FAILED\"}");
}

static int fixture_refresh(Account *account,Accounts *accounts,Tokens *tokens){
	(void)account; (void)accounts;
	assert(fixture_mode==FIXTURE_RETRY_151||fixture_mode==FIXTURE_RETRY_AUTH);
	assert(tokens->id_token&&!strcmp(tokens->id_token,"old-token"));
	fixture_refresh_calls++;
	tokens_free(tokens);
	tokens->id_token=xstrdup("new-token");
	tokens->access_token=xstrdup("new-access");
	tokens->refresh_token=xstrdup("new-refresh");
	return 0;
}

static char *fixture_balance_settlement(const char *path,const char *payload,Tokens *tokens){
	(void)tokens;
	assert(!strcmp(path,"payments/api/v8/settlement-multipayment"));
	if(fixture_mode==FIXTURE_CANONICAL_OPTION){
		assert(payload_has(payload,"\"item_code\":\"canonical-detail-option\""));
		assert(!payload_has(payload,"family-lookup-option"));
		assert(payload_has(payload,"\"is_enterprise\":true"));
		assert(payload_has(payload,"\"migration_type\":\"PRE_TO_PRIOH\""));
		fixture_settlement_calls++;
		return xstrdup("{\"status\":\"SUCCESS\",\"message\":\"canonical fixture settlement\"}");
	}
	assert(fixture_mode==FIXTURE_PAYMENT);
	assert(payload_has(payload,fixture_expected_settlement_enterprise?"\"is_enterprise\":true":"\"is_enterprise\":false"));
	assert(fixture_expected_migration&&payload_has(payload,fixture_expected_migration));
	assert(fixture_expected_payment_for&&payload_has(payload,fixture_expected_payment_for));
	if(fixture_expected_settlement_enterprise){
		assert(payload_has(payload,"\"item_code\":\"decoy-canonical-option\""));
		assert(!payload_has(payload,"decoy-family-lookup-option"));
	}
	fixture_settlement_calls++;
	return xstrdup(fixture_settlement_failure?"{\"status\":\"FAILED\",\"message\":\"declined\"}":"{\"status\":\"SUCCESS\",\"message\":\"fixture settlement\"}");
}

static void reset_fixture(void){
	fixture_mode=FIXTURE_NONE;
	fixture_family_calls=fixture_detail_calls=fixture_refresh_calls=fixture_settlement_calls=0;
	fixture_old_family_calls=fixture_new_family_calls=fixture_account_a_family_calls=fixture_account_b_family_calls=0;
	fixture_expected_method_enterprise=fixture_expected_settlement_enterprise=fixture_settlement_failure=0;
	fixture_retry_151_fail_after_refresh=0;
	fixture_detail_mismatch_kind=0;
	fixture_expected_payment_target=fixture_expected_payment_for=fixture_expected_migration=NULL;
	bitsxl_test_api_request_hook=NULL;
	bitsxl_test_api_retry_auth_hook=NULL;
	bitsxl_test_balance_settlement_hook=NULL;
}

static DecoyConfigChoice choice_for(const char *subscription_type,int expected_result){
	Account account=test_account(subscription_type);
	DecoyConfigChoice choice; memset(&choice,0,sizeof(choice));
	char *error=NULL;
	int result=decoy_config_choice(&account,&choice,&error);
	assert(result==expected_result);
	if(expected_result<0) assert(error&&*error);
	else assert(error==NULL);
	free(error);
	return choice;
}

static void test_subscription_mapping_and_fallback(void){
	reset_decoy_config();
	DecoyConfigChoice choice=choice_for("PREPAID",0);
	assert(!strcmp(choice.category,"PREPAID")&&!choice.configured);
	choice=choice_for("PRIORITAS",0);
	assert(!strcmp(choice.category,"PRIORITAS")&&!choice.prioritas_fallback);
	choice=choice_for("GO",0);
	assert(!strcmp(choice.category,"PRIORITAS")&&!choice.prioritas_fallback);
	choice=choice_for("PRIOHYBRID",0);
	assert(!strcmp(choice.category,"PRIORITAS")&&choice.prioritas_fallback);
	choice=choice_for("NONPRIORITY",0);
	assert(!strcmp(choice.category,"PREPAID"));

	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"prepaid-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"2");
	snprintf(cfg.decoy_prioritas_family_code,sizeof(cfg.decoy_prioritas_family_code),"prio-family");
	snprintf(cfg.decoy_prioritas_package_number,sizeof(cfg.decoy_prioritas_package_number),"3");
	snprintf(cfg.decoy_priohybrid_family_code,sizeof(cfg.decoy_priohybrid_family_code),"hybrid-family");
	snprintf(cfg.decoy_priohybrid_package_number,sizeof(cfg.decoy_priohybrid_package_number),"4");
	choice=choice_for("PREPAID",1);
	assert(!strcmp(choice.family_code,"prepaid-family")&&choice.package_number==2);
	choice=choice_for("PRIORITAS",1);
	assert(!strcmp(choice.family_code,"prio-family")&&choice.package_number==3);
	choice=choice_for("GO",1);
	assert(!strcmp(choice.family_code,"prio-family")&&choice.package_number==3);
	choice=choice_for("PRIOHYBRID",1);
	assert(!strcmp(choice.family_code,"hybrid-family")&&choice.package_number==4&&!choice.prioritas_fallback);

	cfg.decoy_priohybrid_family_code[0]=0;
	cfg.decoy_priohybrid_package_number[0]=0;
	choice=choice_for("PRIOHYBRID",1);
	assert(!strcmp(choice.family_code,"prio-family")&&choice.package_number==3&&choice.prioritas_fallback);

	cfg.decoy_priohybrid_family_code[0]=0;
	snprintf(cfg.decoy_priohybrid_package_number,sizeof(cfg.decoy_priohybrid_package_number),"1");
	choice_for("PRIOHYBRID",-1);
	cfg.decoy_priohybrid_package_number[0]=0;
	cfg.decoy_prioritas_package_number[0]=0;
	choice_for("PRIOHYBRID",-1);
	choice_for("GO",-1);

	reset_decoy_config();
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"not-a-uuid");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"1");
	choice=choice_for("PREPAID",1);
	assert(!strcmp(choice.family_code,"not-a-uuid"));
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"invalid family");
	choice_for("PREPAID",-1);
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"valid-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"0");
	choice_for("PREPAID",-1);
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"2147483648");
	choice_for("PREPAID",-1);
}

static void test_enterprise_context_reaches_detail_and_quote(void){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_CONTEXT;
	bitsxl_test_api_request_hook=fixture_api_request;
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"enterprise-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"2");
	Account account=test_account("PREPAID");
	Tokens tokens={xstrdup("access"),xstrdup("context-token"),xstrdup("refresh"),0};
	PackageContext context={0}; char *error=NULL;
	assert(!decoy_package_context_once(&tokens,&account,&context,&error));
	assert(error==NULL);
	assert(context.resolved);
	assert(!strcmp(context.option_code,"option-B"));
	assert(!strcmp(context.family_code,"enterprise-family"));
	assert(!strcmp(context.variant_code,"variant-B"));
	assert(context.is_enterprise);
	assert(!strcmp(context.migration_type,"PRE_TO_PRIOH"));
	assert(context.package_number==2);
	assert(fixture_family_calls>0);

	PaymentQuote quote={0};
	assert(!payment_quote_load_context(&tokens,&context,&quote,&error));
	assert(error==NULL&&fixture_detail_calls==1);
	assert(quote.context.resolved&&quote.context.is_enterprise);
	assert(!strcmp(quote.context.family_code,context.family_code));
	assert(!strcmp(quote.context.variant_code,context.variant_code));
	assert(!strcmp(quote.context.migration_type,context.migration_type));
	char *response=intercept_page_api_context(&tokens,quote.item_code,&quote.context);
	assert(api_success_response(response)); free(response);
	response=payment_methods_api(&tokens,&quote);
	assert(api_success_response(response)); free(response);
	payment_quote_free(&quote); package_context_free(&context); tokens_free(&tokens);
	reset_fixture(); reset_decoy_config();
}

static void test_builtin_prioritas_preserves_enterprise_context(void){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_BUILTIN_PRIORITAS;
	bitsxl_test_api_request_hook=fixture_api_request;
	Account account=test_account("PRIORITAS");
	Tokens tokens={xstrdup("builtin-prio-access"),xstrdup("builtin-prio-token"),xstrdup("builtin-prio-refresh"),0};
	PackageContext context={0}; char *error=NULL;
	assert(!decoy_package_context_once(&tokens,&account,&context,&error));
	assert(error==NULL&&fixture_family_calls>0);
	assert(context.resolved&&context.is_enterprise&&context.package_number==1);
	assert(!strcmp(context.option_code,"builtin-prio-option"));
	assert(!strcmp(context.family_code,"2512b72a-a3cd-4c70-a736-132cf2c1f0c0"));
	assert(!strcmp(context.variant_code,"cff298bd-8ec8-4696-b689-12407d36be15"));
	assert(!strcmp(context.migration_type,"PRIO_TO_PRIOH"));

	PaymentQuote quote={0};
	assert(!payment_quote_load_context(&tokens,&context,&quote,&error));
	assert(error==NULL&&fixture_detail_calls==1);
	assert(quote.context.resolved&&quote.context.is_enterprise);
	assert(!strcmp(quote.context.family_code,context.family_code));
	assert(!strcmp(quote.context.variant_code,context.variant_code));
	assert(!strcmp(quote.context.migration_type,context.migration_type));
	payment_quote_free(&quote); package_context_free(&context); tokens_free(&tokens);
	reset_fixture(); reset_decoy_config();
}

static void test_account_isolation_uses_each_active_token(void){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_ACCOUNT_ISOLATION;
	bitsxl_test_api_request_hook=fixture_api_request;
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"shared-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"1");
	Account account_a=test_account("PREPAID"),account_b=test_account("PREPAID");
	account_a.subscriber_id=(char *)"subscriber-A";
	account_b.subscriber_id=(char *)"subscriber-B";
	Tokens tokens_a={xstrdup("subscriber-a-access"),xstrdup("subscriber-a-token"),xstrdup("subscriber-a-refresh"),0};
	Tokens tokens_b={xstrdup("subscriber-b-access"),xstrdup("subscriber-b-token"),xstrdup("subscriber-b-refresh"),0};
	PaymentQuote quote_a={0},quote_b={0};
	char *code_a=NULL,*code_b=NULL,*error=NULL;
	assert(!decoy_quote_resolve_once(&account_a,&tokens_a,&quote_a,&code_a,&error));
	assert(error==NULL&&code_a&&!strcmp(code_a,"subscriber-A-option"));
	assert(!decoy_quote_resolve_once(&account_b,&tokens_b,&quote_b,&code_b,&error));
	assert(error==NULL&&code_b&&!strcmp(code_b,"subscriber-B-option"));
	assert(fixture_account_a_family_calls>0&&fixture_account_b_family_calls>0);
	assert(fixture_detail_calls==2&&strcmp(code_a,code_b));
	assert(strcmp(quote_a.context.option_code,quote_b.context.option_code));
	assert(strcmp(quote_a.context.variant_code,quote_b.context.variant_code));
	assert(!strcmp(quote_a.context.family_code,"shared-family"));
	assert(!strcmp(quote_b.context.family_code,"shared-family"));
	free(code_a); free(code_b); payment_quote_free(&quote_a); payment_quote_free(&quote_b); tokens_free(&tokens_a); tokens_free(&tokens_b);
	reset_fixture(); reset_decoy_config();
}

static void test_detail_canonical_option_replaces_family_lookup_option(void){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_CANONICAL_OPTION;
	bitsxl_test_api_request_hook=fixture_api_request;
	bitsxl_test_balance_settlement_hook=fixture_balance_settlement;
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"canonical-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"1");
	Account account=test_account("PREPAID");
	Tokens tokens={xstrdup("canonical-access"),xstrdup("canonical-token"),xstrdup("canonical-refresh"),0};
	PaymentQuote quote={0}; char *resolved_option=NULL,*error=NULL;
	assert(!decoy_quote_resolve_once(&account,&tokens,&quote,&resolved_option,&error));
	assert(error==NULL&&fixture_detail_calls==1&&fixture_settlement_calls==0);
	assert(resolved_option&&!strcmp(resolved_option,"canonical-detail-option"));
	assert(quote.item_code&&!strcmp(quote.item_code,"canonical-detail-option"));
	assert(quote.context.resolved&&quote.context.option_code&&!strcmp(quote.context.option_code,"family-lookup-option"));
	assert(!strcmp(quote.context.family_code,"canonical-family"));
	assert(!strcmp(quote.context.variant_code,"canonical-variant"));
	assert(quote.context.is_enterprise&&!strcmp(quote.context.migration_type,"PRE_TO_PRIOH"));
	assert(quote.token_confirmation&&!strcmp(quote.token_confirmation,"canonical-confirmation"));
	char *response=payment_pulsa_settle_many_ex(&account,&tokens,&quote,1,quote.price,0,NULL);
	assert(api_success_response(response)&&fixture_settlement_calls==1);
	free(response);
	free(resolved_option); payment_quote_free(&quote); tokens_free(&tokens);
	reset_fixture(); reset_decoy_config();
}

static void assert_detail_family_or_variant_mismatch_rejected(int mismatch_kind){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_DETAIL_MISMATCH;
	fixture_detail_mismatch_kind=mismatch_kind;
	bitsxl_test_api_request_hook=fixture_api_request;
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"mismatch-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"1");
	Account account=test_account("PREPAID");
	Tokens tokens={xstrdup("mismatch-access"),xstrdup("mismatch-token"),xstrdup("mismatch-refresh"),0};
	PaymentQuote quote={0}; char *option_code=NULL,*error=NULL;
	assert(decoy_quote_resolve_once(&account,&tokens,&quote,&option_code,&error));
	assert(option_code==NULL&&quote.item_code==NULL&&fixture_detail_calls==1&&fixture_settlement_calls==0);
	assert_error_context(error,"mismatch-family",1,"mismatch-variant",1,"PRIOH_TO_PRIO");
	assert(payload_has(error,"context berbeda"));
	assert(!payload_has(error,"sensitive-confirmation-token"));
	free(error); payment_quote_free(&quote); tokens_free(&tokens);
	reset_fixture(); reset_decoy_config();
}

static void test_invalid_package_and_detail_mismatch_stop_before_settlement(void){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_INVALID_PACKAGE;
	bitsxl_test_api_request_hook=fixture_api_request;
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"invalid-package-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"2");
	Account account=test_account("PREPAID");
	Tokens tokens={xstrdup("invalid-access"),xstrdup("invalid-token"),xstrdup("invalid-refresh"),0};
	PaymentQuote quote={0}; char *option_code=NULL,*error=NULL;
	assert(decoy_quote_resolve_once(&account,&tokens,&quote,&option_code,&error));
	assert(option_code==NULL&&quote.item_code==NULL&&fixture_detail_calls==0&&fixture_settlement_calls==0);
	assert_error_context(error,"invalid-package-family",2,"(unknown)",0,"NONE");
	assert(payload_has(error,"Nomor Paket 2 tidak tersedia/valid"));
	free(error); error=NULL; payment_quote_free(&quote);
	Accounts accounts={0}; PaymentQuote payment_quotes[2]; memset(payment_quotes,0,sizeof(payment_quotes));
	assert(!interactive_payment_decoy_quotes(&accounts,&account,&tokens,payment_quotes,1,0,-1,1));
	assert(fixture_detail_calls==0&&fixture_settlement_calls==0);
	for(size_t i=0;i<2;i++) payment_quote_free(&payment_quotes[i]);
	tokens_free(&tokens);
	assert_detail_family_or_variant_mismatch_rejected(1);
	assert_detail_family_or_variant_mismatch_rejected(2);
}

static void test_payment_context_for_standard_v2_and_ordinary(void){
	reset_fixture();
	fixture_mode=FIXTURE_PAYMENT;
	bitsxl_test_api_request_hook=fixture_api_request;
	bitsxl_test_balance_settlement_hook=fixture_balance_settlement;
	Tokens tokens={xstrdup("payment-access"),xstrdup("payment-id"),xstrdup("payment-refresh"),0};
	PaymentQuote quotes[2]; memset(quotes,0,sizeof(quotes));
	char *error=NULL;
	assert(!payment_quote_load(&tokens,"target-option",&quotes[0],&error));
	assert(error==NULL&&!quotes[0].context.resolved);
	free(quotes[0].payment_for); quotes[0].payment_for=xstrdup("TARGET_PAYMENT");
	Account account; memset(&account,0,sizeof(account));
	fixture_expected_method_enterprise=0;
	fixture_expected_settlement_enterprise=0;
	fixture_expected_payment_target="\"payment_target\":\"target-option\"";
	fixture_expected_payment_for="\"payment_for\":\"TARGET_PAYMENT\"";
	fixture_expected_migration="\"migration_type\":\"\"";
	char *response=payment_pulsa_settle_many_ex(&account,&tokens,quotes,1,1000,0,NULL);
	assert(api_success_response(response)); free(response);

	quotes[1].item_code=xstrdup("decoy-canonical-option");
	quotes[1].item_name=xstrdup("Decoy");
	quotes[1].family=xstrdup("Decoy Family");
	quotes[1].validity=xstrdup("1 day");
	quotes[1].payment_for=xstrdup("DECOY_PAYMENT");
	quotes[1].token_confirmation=xstrdup("decoy-confirmation");
	quotes[1].price=1000; quotes[1].balance=-1; quotes[1].timestamp=123456;
	package_context_set(&quotes[1].context,"decoy-family-lookup-option","enterprise-family","enterprise-variant",1,"PRIOH_TO_PRIO",2,1);
	assert(strcmp(quotes[1].item_code,quotes[1].context.option_code));

	fixture_expected_method_enterprise=0;
	fixture_expected_settlement_enterprise=1;
	fixture_expected_payment_target="\"payment_target\":\"target-option\"";
	fixture_expected_payment_for="\"payment_for\":\"TARGET_PAYMENT\"";
	fixture_expected_migration="\"migration_type\":\"PRIOH_TO_PRIO\"";
	response=payment_pulsa_settle_many_ex(&account,&tokens,quotes,2,2000,0,NULL);
	assert(api_success_response(response)); free(response);

	fixture_expected_method_enterprise=1;
	fixture_expected_payment_target="\"payment_target\":\"decoy-canonical-option\"";
	fixture_expected_payment_for=DECOY_V2_PAYMENT_FOR;
	response=payment_pulsa_settle_many_ex(&account,&tokens,quotes,2,2000,1,DECOY_V2_PAYMENT_FOR);
	assert(api_success_response(response)); free(response);
	fixture_settlement_failure=1;
	response=payment_pulsa_settle_many_ex(&account,&tokens,quotes,2,2000,1,DECOY_V2_PAYMENT_FOR);
	assert(!api_success_response(response));
	assert(payload_has(response,"\"family_code\":\"enterprise-family\""));
	assert(payload_has(response,"\"package_number\":2"));
	assert(payload_has(response,"\"variant_code\":\"enterprise-variant\""));
	assert(payload_has(response,"\"is_enterprise\":true"));
	assert(payload_has(response,"\"migration_type\":\"PRIOH_TO_PRIO\""));
	free(response);
	assert(fixture_settlement_calls==4);
	assert(!strcmp(DECOY_V2_PAYMENT_FOR,"\xF0\x9F\xA4\xAB"));
	for(size_t i=0;i<2;i++) payment_quote_free(&quotes[i]);
	tokens_free(&tokens); reset_fixture();
}

static void test_error_151_re_resolves_full_context_once(void){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_RETRY_151;
	bitsxl_test_api_request_hook=fixture_api_request;
	bitsxl_test_api_retry_auth_hook=fixture_refresh;
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"retry-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"1");
	Account account=test_account("PREPAID"); Accounts accounts={0};
	Tokens tokens={xstrdup("old-access"),xstrdup("old-token"),xstrdup("old-refresh"),0};
	PaymentQuote quote={0}; char *option_code=NULL,*error=NULL; int refreshed=0;
	assert(!decoy_quote_resolve(&accounts,&account,&tokens,&quote,&option_code,&error,&refreshed));
	assert(error==NULL&&refreshed==1);
	assert(fixture_refresh_calls==1&&fixture_detail_calls==2);
	assert(fixture_old_family_calls>0&&fixture_new_family_calls>0);
	assert(option_code&&!strcmp(option_code,"new-option"));
	assert(quote.context.resolved&&!quote.context.is_enterprise);
	assert(!strcmp(quote.context.option_code,"new-option"));
	assert(!strcmp(quote.context.family_code,"retry-family"));
	assert(!strcmp(quote.context.variant_code,"new-variant"));
	assert(!strcmp(quote.context.migration_type,"PRE_TO_PRIOH"));
	free(option_code); payment_quote_free(&quote); tokens_free(&tokens);
	reset_fixture(); reset_decoy_config();
}

static void test_error_151_retry_failure_is_bounded_and_contextual(void){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_RETRY_151;
	fixture_retry_151_fail_after_refresh=1;
	bitsxl_test_api_request_hook=fixture_api_request;
	bitsxl_test_api_retry_auth_hook=fixture_refresh;
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"retry-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"1");
	Account account=test_account("PREPAID"); Accounts accounts={0};
	Tokens tokens={xstrdup("old-access"),xstrdup("old-token"),xstrdup("old-refresh"),0};
	PaymentQuote quote={0}; char *option_code=NULL,*error=NULL; int refreshed=0;
	assert(decoy_quote_resolve(&accounts,&account,&tokens,&quote,&option_code,&error,&refreshed));
	assert(refreshed==1&&fixture_refresh_calls==1&&fixture_detail_calls==2);
	assert(fixture_old_family_calls>0&&fixture_new_family_calls>0);
	assert(option_code==NULL&&quote.item_code==NULL&&fixture_settlement_calls==0);
	assert(payload_has(error,"tetap gagal setelah error 151"));
	assert_error_context(error,"retry-family",1,"new-variant",0,"PRE_TO_PRIOH");
	free(error); payment_quote_free(&quote); tokens_free(&tokens);
	reset_fixture(); reset_decoy_config();
}

static void test_auth_failure_refreshes_then_resolves_context(void){
	reset_fixture(); reset_decoy_config();
	fixture_mode=FIXTURE_RETRY_AUTH;
	bitsxl_test_api_request_hook=fixture_api_request;
	bitsxl_test_api_retry_auth_hook=fixture_refresh;
	snprintf(cfg.decoy_prepaid_family_code,sizeof(cfg.decoy_prepaid_family_code),"auth-family");
	snprintf(cfg.decoy_prepaid_package_number,sizeof(cfg.decoy_prepaid_package_number),"1");
	Account account=test_account("PREPAID"); Accounts accounts={0};
	Tokens tokens={xstrdup("old-access"),xstrdup("old-token"),xstrdup("old-refresh"),0};
	PaymentQuote quote={0}; char *option_code=NULL,*error=NULL; int refreshed=0;
	assert(!decoy_quote_resolve(&accounts,&account,&tokens,&quote,&option_code,&error,&refreshed));
	assert(error==NULL&&refreshed==1&&fixture_refresh_calls==1);
	assert(fixture_old_family_calls>0&&fixture_new_family_calls>0&&fixture_detail_calls==1);
	assert(tokens.id_token&&!strcmp(tokens.id_token,"new-token"));
	assert(option_code&&!strcmp(option_code,"auth-option"));
	assert(quote.context.resolved&&quote.context.is_enterprise&&quote.context.package_number==1);
	assert(!strcmp(quote.context.family_code,"auth-family"));
	assert(!strcmp(quote.context.variant_code,"auth-variant"));
	assert(!strcmp(quote.context.migration_type,"PRE_TO_PRIOH"));
	free(option_code); payment_quote_free(&quote); tokens_free(&tokens);
	reset_fixture(); reset_decoy_config();
}

static void test_flattened_package_number_and_variant(void){
	const char *family_response=
		"{\"status\":\"SUCCESS\",\"data\":{\"package_variants\":["
		"{\"package_variant_code\":\"variant-A\",\"package_options\":[{\"order\":1,\"package_option_code\":\"A1\"},{\"order\":1,\"package_option_code\":\"A2\"}]},"
		"{\"package_variant_code\":\"variant-B\",\"package_options\":[{\"order\":1,\"package_option_code\":\"B1\"}]}]}}";
	char *error=NULL; int total=0;
	char *code=flattened_family_option_code(family_response,1,&total,&error);
	assert(code&&!strcmp(code,"A1")&&total==3&&error==NULL); free(code);
	code=flattened_family_option_code(family_response,2,&total,&error);
	assert(code&&!strcmp(code,"A2")&&total==3&&error==NULL); free(code);
	code=flattened_family_option_code(family_response,3,&total,&error);
	assert(code&&!strcmp(code,"B1")&&total==3&&error==NULL); free(code);
	code=flattened_family_option_code(family_response,4,&total,&error);
	assert(code==NULL&&total==3&&error); free(error); error=NULL;
	PackageContext selected={0};
	package_context_set(&selected,"","family","",1,"PRIOH_TO_PRIO",0,0);
	assert(!flattened_family_package_context(family_response,3,&selected,&total,&error));
	assert(selected.resolved&&selected.package_number==3);
	assert(!strcmp(selected.option_code,"B1")&&!strcmp(selected.variant_code,"variant-B"));
	assert(selected.is_enterprise&&!strcmp(selected.migration_type,"PRIOH_TO_PRIO"));
	package_context_free(&selected);

}

static void test_error_codes_and_utf8_literals(void){
	assert(api_error_151("{\"code\":\"151\",\"message\":\"GENERAL_ERROR_HANDLER\"}"));
	assert(api_error_151("{\"code\":151}"));
	assert(!api_error_151("{\"code\":150}"));
	assert(!api_error_151("{\"code\":1510}"));
	assert(!api_error_151("status=FAILED code=151"));
	assert(api_auth_failed("status=FAILED code=132 auth_error=true"));
	assert(api_auth_failed("status=FAILED code=REQUEST_MISSING_BEARER auth_error=true"));
	assert(!strcmp(DECOY_V2_PAYMENT_FOR,"\xF0\x9F\xA4\xAB"));
	assert(!strcmp(FAMILY_LOOP_PAYMENT_FOR,"\xF0\x9F\xA4\x91"));
}

static void test_family_start_uses_api_order(void){
	QuotaList options={0}; size_t index=99;
	ql_add(&options,xstrdup("first"),xstrdup("variant-a"),xstrdup("code-a"),xstrdup("1"),xstrdup("variant-a-code"));
	ql_add(&options,xstrdup("second"),xstrdup("variant-a"),xstrdup("code-b"),xstrdup("2"),xstrdup("variant-a-code"));
	ql_add(&options,xstrdup("third"),xstrdup("variant-b"),xstrdup("code-c"),xstrdup("1"),xstrdup("variant-b-code"));
	assert(!family_purchase_start_index(&options,1,&index)&&index==0);
	assert(!family_purchase_start_index(&options,2,&index)&&index==1);
	assert(family_purchase_start_index(&options,3,&index));
	ql_free(&options);
}

int main(void){
	test_subscription_mapping_and_fallback();
	test_enterprise_context_reaches_detail_and_quote();
	test_builtin_prioritas_preserves_enterprise_context();
	test_account_isolation_uses_each_active_token();
	test_detail_canonical_option_replaces_family_lookup_option();
	test_invalid_package_and_detail_mismatch_stop_before_settlement();
	test_payment_context_for_standard_v2_and_ordinary();
	test_error_151_re_resolves_full_context_once();
	test_error_151_retry_failure_is_bounded_and_contextual();
	test_auth_failure_refreshes_then_resolves_context();
	test_flattened_package_number_and_variant();
	test_error_codes_and_utf8_literals();
	test_family_start_uses_api_order();
	puts("decoy resolver tests passed");
	return 0;
}
