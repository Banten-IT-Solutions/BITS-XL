#define BITSXL_TEST 1
#define main bitsxl_program_main
#include "../../src/bitsxl.c"
#undef main

#include <assert.h>

static void assert_login_number(const char *input,const char *expected){
	char *number=login_msisdn(input);
	assert(number);
	assert(!strcmp(number,expected));
	free(number);
}

int main(void){
	assert_login_number("081234567890","6281234567890");
	assert_login_number("81234567890","6281234567890");
	assert_login_number("6281234567890","6281234567890");
	assert_login_number(" +62 812-3456-7890 ","6281234567890");
	assert(!login_msisdn("071234567890"));
	assert(!login_msisdn("08"));
	assert(!login_msisdn("not-a-number"));
	puts("MSISDN normalization tests passed");
	return 0;
}
