#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpostal/libpostal.h>

// test running helper
void run_test(libpostal_address_parser_options_t options, const char *address, const char *expected_code) {
    printf("------------------------------------------------\n");
    printf("Input:    %s\n", address);

    libpostal_address_parser_response_t *parsed = libpostal_parse_address((char *)address, options);
    
    char *actual_code = parsed->country_guess;

    // determine if test passed
    int passed = 0;
    if (expected_code == NULL) {
        if (actual_code == NULL) passed = 1;
    } else {
        if (actual_code != NULL && strcmp(actual_code, expected_code) == 0) passed = 1;

    }

    if (passed) {
        printf("Result:   %s\n", actual_code ? actual_code : "NULL");
        printf("Status:   \033[0;32m[PASS]\033[0m\n"); // green = pass
    } else {
        printf("Expected: %s\n", expected_code ? expected_code : "NULL");
        printf("Actual:   %s\n", actual_code ? actual_code : "NULL");
        printf("Status:   \033[0;31m[FAIL]\033[0m\n"); 
    }

    libpostal_address_parser_response_destroy(parsed);
}

int main(int argc, char **argv) {
    if (!libpostal_setup() || !libpostal_setup_parser()) {
        exit(EXIT_FAILURE);
    }
    libpostal_address_parser_options_t options = libpostal_get_address_parser_default_options();

    printf("\nRUNNING LIBPOSTAL COUNTRY GUESS TESTS\n");
    printf("================================================\n");

    // ---------------------------------------------------------
    // US
    // ---------------------------------------------------------
    // test 1: New York (NY)
    run_test(options, "781 Franklin Ave Crown Heights Brooklyn NY 11216", "US");

    // test 2: Florida (FL)
    run_test(options, "123 Ocean Drive, Miami, FL 33139", "US");

    // test 3: California (CA) - Note: 'CA' is also Canada, but usually state context implies US state first in heuristics if labelled 'state'
    run_test(options, "1 Infinite Loop, Cupertino, CA 95014", "US");

    // ---------------------------------------------------------
    // CANADA
    // ---------------------------------------------------------
    // test 4: British Columbia (BC)
    run_test(options, "332 Menzies Street, Victoria, BC V8V 2G9", "CA");

    // test 5: Ontario (ON)
    run_test(options, "290 Bremner Blvd, Toronto, ON M5V 3L9", "CA");

    // test 6: Quebec (QC)
    run_test(options, "3800 Chemin Queen Mary, Montreal, QC H3V 1H6", "CA");

    // ---------------------------------------------------------
    // AUSTRALIA 
    // ---------------------------------------------------------
    // test 7: New South Wales (NSW)
    run_test(options, "Bennelong Point, Sydney NSW 2000", "AU");

    // test 8: Victoria (VIC)
    run_test(options, "Flinders St, Melbourne VIC 3000", "AU");

    // ---------------------------------------------------------
    // UNITED KINGDOM (Postcode/Explicit)
    // ---------------------------------------------------------
    // test 9: London (Explicit 'UK')
    run_test(options, "Buckingham Palace, London, UK", "GB");

    // test 10: London (Postcode Heuristic 'SW1A')
    run_test(options, "10 Downing St, London, SW1A 2AA", "GB");

    // ---------------------------------------------------------
    // EXPLICIT COUNTRIES
    // ---------------------------------------------------------
    // test 11: 'United States' -> should normalize to 'US'
    run_test(options, "1600 Pennsylvania Ave NW, Washington, DC, United States", "US");

    // test 12: 'USA' -> should normalize to 'US'
    run_test(options, "Space Needle, Seattle, WA, USA", "US");

    libpostal_teardown();
    libpostal_teardown_parser();

    printf("================================================\n");
    return 0;
}
