#!/usr/bin/env tarantool

local replication = os.getenv("MASTER")
if arg[1] == 'disable_replication' then
    replication = nil
end

box.cfg({
    replication     = replication,
    vinyl_memory    = 107374182,
})

require('console').listen(os.getenv('ADMIN'))
