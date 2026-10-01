/* pol-signin: sign in to a PlayOnline server and obtain the FFXI session value, as the sign-in screen
 * does before it starts the game.
 *
 *   pol-signin [--host 127.0.0.1] [--hold <seconds>] <PlayOnline ID> [password]
 *
 * Prints V as 32 hex digits (what the game host takes as --session). With --hold it stays signed
 * in - answering the server's keepalive - for that many seconds, as the client does while the
 * game runs; V is only valid while the server counts the sign-in as recent. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "polsession.h"

int main(int argc, char** argv)
{
    const char* host = "127.0.0.1";
    const char* pol_id = NULL;
    const char* password = "";
    int hold = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--host") && i + 1 < argc)
            host = argv[++i];
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc)
            hold = atoi(argv[++i]);
        else if (!pol_id)
            pol_id = argv[i];
        else
            password = argv[i];
    }
    if (!pol_id)
    {
        fprintf(stderr, "usage: pol-signin [--host h] [--hold seconds] <PlayOnline ID> [password]\n");
        return 2;
    }

    static PolSession s;
    if (!pol_signin(&s, host, POL_PORT_IRC, pol_id, password))
    {
        fprintf(stderr, "sign-in failed: %s\n", s.error);
        return 1;
    }
    fprintf(stderr, "signed in as %s (nick %s)\n", s.pol_id, s.nick);

    uint8_t v[16];
    if (!pol_ffxi_session_value(&s, POL_PORT_POLPRO, v))
    {
        fprintf(stderr, "no session value: %s\n", s.error);
        pol_signout(&s);
        return 1;
    }
    for (int i = 0; i < 16; ++i)
        printf("%02x", v[i]);
    printf("\n");
    fflush(stdout);

    time_t until = time(NULL) + hold;
    while (time(NULL) < until)
        if (!pol_pump(&s, 1000))
        {
            fprintf(stderr, "the server closed the session\n");
            return 1;
        }
    pol_signout(&s);
    return 0;
}
