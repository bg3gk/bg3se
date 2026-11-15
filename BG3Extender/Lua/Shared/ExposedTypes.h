#pragma once

BEGIN_NS(lua)

struct EntityTracingOptions
{
    bool TrackECB{ true };
    bool TrackImmediateWorldCache{ true };
    bool TrackReplication{ true };
    bool TrackModifications{ false };
    Array<STDString> ExcludeModificationComponents;
    // Filters applied at the source: only these entities (empty: every entity) and only these
    // component types (empty: every type), engine names or Lua labels
    Array<EntityHandle> Entities;
    Array<STDString> Components;
};

END_NS()
