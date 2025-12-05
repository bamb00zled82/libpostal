#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpostal/libpostal.h>

// method for testing if the country code guess heuristic is working
int main(int argc, char **argv) {
    if (!libpostal_setup() || !libpostal_setup_parser()) exit(EXIT_FAILURE);

    libpostal_address_parser_options_t options = libpostal_get_address_parser_default_options();

    // test case 1: New York (should guess US)
    char *addr = "781 Franklin Ave Crown Heights Brooklyn NY 11216";
    printf("Parsing: %s\n", addr);

    libpostal_address_parser_response_t *parsed = libpostal_parse_address(addr, options);

    printf("Guessed Country: %s\n", parsed->country_guess ? parsed->country_guess : "Unknown");

    // test case 2: British Columbia (should guess CA)
    char *addr2 = "332 Menzies Street, Victoria, BC V8V 2G9";
    printf("\nParsing: %s\n", addr2);

    libpostal_address_parser_response_t *parsed2 = libpostal_parse_address(addr2, options);
    printf("Guessed Country: %s\n", parsed2->country_guess ? parsed2->country_guess : "Unknown");

    libpostal_address_parser_response_destroy(parsed);
    libpostal_address_parser_response_destroy(parsed2);
    libpostal_teardown();
    libpostal_teardown_parser();

    printf("================================================\n");
    return 0;
}
