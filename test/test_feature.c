#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpostal/libpostal.h>

// Helper to run and print a test
void run_test(libpostal_address_parser_options_t options,
              const char *address,
              const char *expected_code) {
    printf("------------------------------------------------\n");
    printf("Input:    %s\n", address);

    libpostal_address_parser_response_t *parsed =
        libpostal_parse_address((char *)address, options);

    if (parsed == NULL) {
        printf("Parser returned NULL\n");
        printf("Status:   \033[0;31m[FAIL]\033[0m\n");
        return;
    }

    char *actual = parsed->country_guess;

    int passed = 0;

    if (expected_code == NULL) {
        if (actual == NULL) passed = 1;
    } else {
        if (actual != NULL && strcmp(actual, expected_code) == 0)
            passed = 1;
    }

    if (passed) {
        printf("Result:   %s\n", actual ? actual : "NULL");
        printf("Status:   \033[0;32m[PASS]\033[0m\n");
    } else {
        printf("Expected: %s\n", expected_code ? expected_code : "NULL");
        printf("Actual:   %s\n", actual ? actual : "NULL");
        printf("Status:   \033[0;31m[FAIL]\033[0m\n");
    }

    libpostal_address_parser_response_destroy(parsed);
}

int main(int argc, char **argv) {
    if (!libpostal_setup() || !libpostal_setup_parser()) {
        exit(EXIT_FAILURE);
    }

    libpostal_address_parser_options_t options =
        libpostal_get_address_parser_default_options();

    printf("\nRUNNING LIBPOSTAL COUNTRY GUESS TESTS\n");
    printf("================================================\n");

    // UNITED STATES (US)
    run_test(options, "781 Franklin Ave Crown Heights Brooklyn NY 11216", "US");
    run_test(options, "123 Ocean Drive, Miami, FL 33139", "US");
    run_test(options, "1 Infinite Loop, Cupertino, CA 95014", "US");

    // CANADA (CA)
    run_test(options, "332 Menzies Street, Victoria, BC V8V 2G9", "CA");
    run_test(options, "290 Bremner Blvd, Toronto, ON M5V 3L9", "CA");
    run_test(options, "3800 Chemin Queen Mary, Montreal, QC H3V 1H6", "CA");

    // AUSTRALIA (AU)
    run_test(options, "Bennelong Point, Sydney NSW 2000", "AU");
    run_test(options, "Flinders St, Melbourne VIC 3000", "AU");

    // UNITED KINGDOM (GB)
    run_test(options, "Buckingham Palace, London, UK", "GB");
    run_test(options, "10 Downing St, London, SW1A 2AA", "GB");

    // EXPLICIT COUNTRIES (US)
    run_test(options,
             "1600 Pennsylvania Ave NW, Washington, DC, United States",
             "US");

    run_test(options,
             "Space Needle, Seattle, WA, USA",
             "US");

    // NEW FEATURE: EXPLICIT COUNTRY NAME MAPPING
    run_test(options,
             "Brandenburger Tor, Pariser Platz, Berlin, Germany",
             "DE");

    run_test(options,
             "Hauptbahnhof, Frankfurt am Main, Deutschland",
             "DE");

    run_test(options,
             "Avenue des Champs-Élysées, Paris, France",
             "FR");

    run_test(options,
             "Rajpath, New Delhi, India",
             "IN");

    // NEW FEATURE: options.country PRIORITY
    libpostal_address_parser_options_t options_us =
        libpostal_get_address_parser_default_options();
    options_us.country = "US";
    run_test(options_us, "Main Street 123", "US");

    // NEW FEATURE: ZIP-ONLY / POSTCODE-ONLY HEURISTICS

    // US ZIP only
    run_test(options, "62704", "US");

    // Canada A1A 1A1 only
    run_test(options, "V8V 2G9", "CA");
    run_test(options, "M5V 3L9", "CA");

    // UK postcode only
    run_test(options, "NW1 6XE", "GB");
    run_test(options, "SW1A 2AA", "GB");

    // NO GUESS EXPECTED
    run_test(options, "Just a random building with no country", NULL);

    libpostal_teardown();
    libpostal_teardown_parser();

    printf("================================================\n");
    return 0;
}
