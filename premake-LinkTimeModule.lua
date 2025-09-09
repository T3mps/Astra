local p = premake

-- Register API (use 'field' for compatibility with older Premake)
p.api.register {
    name = "linktimeoptimization",
    scope = "config",
    kind = "string",
    allowed = {
        "On",
        "Off",
    }
}

-- Try to override getldflags for both c and cpp modules if available
local function add_lto_override(module)
    if module and module.getldflags then
        p.override(module, "getldflags", function(base, cfg)
            local flags = base(cfg)
            if cfg.linktimeoptimization == "On" then
                if cfg.system == p.WINDOWS then
                    table.insert(flags, "/LTCG")
                elseif cfg.system == p.LINUX or cfg.system == p.MACOSX then
                    table.insert(flags, "-flto")
                end
            end
            return flags
        end)
    end
end

add_lto_override(p.modules.c)
add_lto_override(p.modules.cpp)