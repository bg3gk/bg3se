#pragma once

BEGIN_NS(lua)

struct EmptyEventParams : public lua::EventBase
{
};

struct DoConsoleCommandEventParams : public lua::EventBase
{
    [[bg3::readonly]] STDString Command;
};

struct ConsoleMessageEventParams : public lua::EventBase
{
    // Running number of the message; a gap means messages were dropped
    [[bg3::readonly]] uint64_t Index;
    // "Debug", "Info", "Osiris", "Warning" or "Error"
    [[bg3::readonly]] FixedString Type;
    [[bg3::readonly]] STDString Message;
};

struct TickEventParams : public lua::EventBase
{
    GameTime Time;
};

struct NetMessageEventParams : public lua::EventBase
{
    StringView Channel;
    StringView Payload;
    std::optional<Guid> Module;
    std::optional<net::RequestId> RequestId;
    UserId UserID;
    bool Binary{ false };
};

struct FindPathEventParams : public lua::EventBase
{
    AiPath* Path{ nullptr };
};

END_NS()
