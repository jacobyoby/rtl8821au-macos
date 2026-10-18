#include <stdio.h>
#include "rtl_ccmp.h"
int main(void) {
    int rc = rtl_ccmp_selftest();
    printf("AES-CCM self-test (NIST SP 800-38C, example 1): %s\n", rc==0 ? "PASS" : "FAIL");
    return rc;
}
