#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "pink/include/redis_conn.h"
#include <ndbapi/NdbApi.hpp>
#include <ndbapi/Ndb.hpp>

#include "db_interactions.h"

/*
    All STRING commands: https://redis.io/docs/latest/commands/?group=string
*/

void rondb_get_command(Ndb *ndb,
                       const pink::RedisCmdArgsType &argv,
                       std::string *response);

void rondb_set_command(Ndb *ndb,
                       const pink::RedisCmdArgsType &argv,
                       std::string *response);
