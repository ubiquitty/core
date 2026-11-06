// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Protocol/Control/MethodRegistry.cpp — see MethodRegistry.h.

#include "Core/Protocol/Control/MethodRegistry.h"

#include "Core/Protocol/Control/JSONSchema.h"

#include <array>
#include <string>
#include <utility>

namespace Ubiquitty::Protocol::Control {

namespace {

using Codec::JSON::Value;
using Sync::Role;
using Sync::Roles;

constexpr Principals controlCallers { Principal::LocalProcess };
constexpr Principals commandLineCallers { Principal::Person, Principal::Agent };
constexpr Principals agentCallers { Principal::Agent };

// Who may call over Sync. Reading what an attachment is already shown is
// open to every role; what drives a pane is closed to a viewer.
constexpr Roles anyAttachment { Role::Owner, Role::Driver, Role::Viewer, Role::Agent };
constexpr Roles drivingAttachments { Role::Owner, Role::Driver, Role::Agent };
// Generic consent belongs to the subscribed person, including a viewer.
// Structure's stronger Owner/Driver and Drive checks are mandatory at the
// kind-bound pending-operation boundary, not relaxed by this shared entry.
constexpr Roles humanAttachments { Role::Owner, Role::Driver, Role::Viewer };

// An agent operation's result is what the MCP server returns for it: content
// blocks, flagged when the call failed. The command line prints the same text.
constexpr std::string_view toolResultSchema = R"json({"type":"object","properties":{"content":{"type":"array","items":{"type":"object","properties":{"type":{"type":"string","enum":["text","image"]},"text":{"type":"string"},"data":{"type":"string"},"mimeType":{"type":"string"}},"required":["type"]}},"isError":{"type":"boolean"}},"required":["content"]})json";

constexpr std::array registry {
    MethodDescriptor {
        .name = "ping",
        .description = "Answers with the server's name and the control methods it serves, without touching any window.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"name":{"type":"string"},"methods":{"type":"array","items":{"type":"string"}}},"required":["name","methods"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"name":"ubiquitty-control","methods":["ping","ls"]}})json",
        .principals = controlCallers,
        .controlMethod = "ping",
        .controlRoute = ControlRoute::Handshake,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "initialize",
        .description = "Opens a JSON-RPC session, answering as ping does; a client's handshake fields are accepted and read by nothing.",
        .parametersSchema = R"json({"type":"object"})json",
        .resultSchema = R"json({"type":"object","properties":{"name":{"type":"string"},"methods":{"type":"array","items":{"type":"string"}}},"required":["name","methods"],"additionalProperties":false})json",
        .example = R"json({"params":{"clientInfo":{"name":"script"}},"result":{"name":"ubiquitty-control","methods":["ping","ls"]}})json",
        .principals = controlCallers,
        .controlMethod = "initialize",
        .controlRoute = ControlRoute::Handshake,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "list",
        .description = "Lists every operation the caller may invoke, with a sentence saying what each does.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"methods":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"description":{"type":"string"},"controlMethod":{"type":"string"},"commandLineVerb":{"type":"string"},"mcpTool":{"type":"string"},"syncMethod":{"type":"string"}},"required":["name","description"],"additionalProperties":false}}},"required":["methods"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"methods":[{"name":"ls","description":"Lists every window.","controlMethod":"ls"}]}})json",
        .principals = controlCallers,
        .controlMethod = "list",
        .controlRoute = ControlRoute::ListOperations,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "describe",
        .description = "Describes one operation: its parameter and result schemas, an example, who may call it, and the name each surface reaches it by.",
        .parametersSchema = R"json({"type":"object","properties":{"method":{"type":"string","minLength":1,"description":"Any name the operation is reached by: the registry's, the control method, the MCP tool, or the command-line verb."}},"required":["method"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"name":{"type":"string"},"description":{"type":"string"},"params":{"type":"object"},"result":{"type":"object"},"example":{"type":"object"},"principals":{"type":"array","items":{"type":"string","enum":["person","localProcess","agent"]}},"controlMethod":{"type":"string"},"commandLineVerb":{"type":"string"},"mcpTool":{"type":"string"},"syncMethod":{"type":"string"},"roles":{"type":"array","items":{"type":"string","enum":["owner","driver","viewer","agent"]}},"scope":{"type":"string","enum":["observe","drive","control","region","new","file","manage"]},"target":{"type":"string","enum":["pane","host","connection"]},"commandLineScopes":{"type":"array","items":{"type":"string","enum":["observe","drive","control","region","new","file","manage"]}},"origin":{"type":"string","enum":["local"]}},"required":["name","description","params","result","example","principals"],"additionalProperties":false})json",
        .example = R"json({"params":{"method":"ls"},"result":{"name":"ls","description":"Lists every window.","params":{"type":"object"},"result":{"type":"object"},"example":{"params":{},"result":{"windows":[]}},"principals":["localProcess"],"controlMethod":"ls"}})json",
        .principals = controlCallers,
        .controlMethod = "describe",
        .controlRoute = ControlRoute::DescribeOperation,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "ls",
        .description = "Lists every window with its tabs and the panes each tab holds.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"windows":{"type":"array","items":{"type":"object","properties":{"id":{"type":"integer"},"title":{"type":"string"},"focused":{"type":"boolean"},"remote":{"type":"boolean"},"daemonSessionID":{"type":"integer"},"tabs":{"type":"array","items":{"type":"object"}}},"required":["id","tabs"]}}},"required":["windows"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"windows":[{"id":4,"title":"zsh","focused":true,"remote":false,"tabs":[{"index":0,"windowID":4,"title":"zsh","selected":true,"panes":[{"id":1,"focused":true,"columns":80,"rows":24,"title":"zsh"}]}]}]}})json",
        .principals = controlCallers,
        .controlMethod = "ls",
        .controlRoute = ControlRoute::Host,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "focus",
        .description = "Raises a window, selects its tab, and focuses a pane in it.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A window ID from ls; 0 or absent means the key window."},"pane":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A pane ID from ls; 0 or absent leaves the window's focus."}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"focused":{"type":"boolean"},"window":{"type":"integer"}},"required":["focused","window"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":4,"pane":2},"result":{"focused":true,"window":4}})json",
        .principals = controlCallers,
        .controlMethod = "focus",
        .controlRoute = ControlRoute::Host,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "send-text",
        .description = "Types text verbatim into a pane's program, once the agent policy allows the line.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A window ID from ls; 0 or absent means the key window, or the window holding pane."},"pane":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A pane ID from ls; 0 or absent means the window's focused pane."},"text":{"type":"string","minLength":1,"description":"The bytes to type; a trailing newline runs a command line."}},"required":["text"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sent":{"type":"integer","minimum":0}},"required":["sent"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":2,"text":"make test\n"},"result":{"sent":10}})json",
        .principals = controlCallers,
        .controlMethod = "send-text",
        .controlRoute = ControlRoute::HostTypesText,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "get-text",
        .description = "Reads a pane's visible screen or its selection as text, once the agent policy allows the read.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A window ID from ls; 0 or absent means the key window, or the window holding pane."},"pane":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A pane ID from ls; 0 or absent means the window's focused pane."},"source":{"type":"string","enum":["screen","selection"],"default":"screen"}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"source":{"type":"string","enum":["screen","selection"]},"text":{"type":"string"}},"required":["source","text"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":2,"source":"screen"},"result":{"source":"screen","text":"$ make test\nok\n"}})json",
        .principals = controlCallers,
        .controlMethod = "get-text",
        .controlRoute = ControlRoute::HostReadsScreen,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "new-window",
        .description = "Opens a window running the login shell, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"integer"}},"required":["window"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"window":7}})json",
        .principals = controlCallers,
        .controlMethod = "new-window",
        .controlRoute = ControlRoute::HostOpens,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "new-tab",
        .description = "Opens a tab beside the key window's, or a window when none is open, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"integer"}},"required":["window"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"window":8}})json",
        .principals = controlCallers,
        .controlMethod = "new-tab",
        .controlRoute = ControlRoute::HostOpens,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "split",
        .description = "Splits a window's focused pane to the right or downward, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A window ID from ls; 0 or absent means the key window."},"direction":{"type":"string","enum":["right","down"],"default":"right"}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"integer"}},"required":["window"],"additionalProperties":false})json",
        .example = R"json({"params":{"direction":"down"},"result":{"window":4}})json",
        .principals = controlCallers,
        .controlMethod = "split",
        .controlRoute = ControlRoute::HostSplits,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "set-title",
        .description = "Pins a window's title, or clears the pin when the title is empty.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A window ID from ls; 0 or absent means the key window."},"title":{"type":"string","description":"The title; empty returns the window to its automatic one."}},"required":["title"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"integer"},"title":{"type":"string"}},"required":["window","title"],"additionalProperties":false})json",
        .example = R"json({"params":{"title":"build"},"result":{"window":4,"title":"build"}})json",
        .principals = controlCallers,
        .controlMethod = "set-title",
        .controlRoute = ControlRoute::Host,
        .commandLineVerb = "",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "terminal.new",
        .description = "Creates a session running the login shell on the daemon and returns its ID, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","properties":{"columns":{"type":"integer","minimum":1,"maximum":1000,"default":80},"rows":{"type":"integer","minimum":1,"maximum":1000,"default":24}},"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"columns":120,"rows":40},"result":{"content":[{"type":"text","text":"session 3"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_new",
    },
    MethodDescriptor {
        .name = "terminal.screen",
        .description = "Reads a session's screen as UTF-8 text, box drawing and full-screen layouts included.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295,"description":"A session ID from terminal_new or terminal_list."},"format":{"type":"string","enum":["text","structured"],"default":"text"}},"required":["session"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"session":3},"result":{"content":[{"type":"text","text":"$ ls\nREADME.md\n"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_screen",
    },
    MethodDescriptor {
        .name = "terminal.changes",
        .description = "Reports the rows of a session's screen that changed since this caller last read it.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295,"description":"A session ID from terminal_new or terminal_list."}},"required":["session"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"session":3},"result":{"content":[{"type":"text","text":"(no change)"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_changes",
    },
    MethodDescriptor {
        .name = "terminal.run",
        .description = "Runs a command line in a session and returns the settled screen with the command's exit status, once the agent policy allows the line.",
        .parametersSchema = R"json({"type":"object","properties":{"command":{"type":"string","minLength":1,"description":"The command line to run."},"session":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A session ID; 0 or absent creates one."},"timeout_ms":{"type":"integer","minimum":0,"maximum":2147483647,"default":10000}},"required":["command"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"command":"make test","session":3},"result":{"content":[{"type":"text","text":"ok\n[exit=0]"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_run",
    },
    MethodDescriptor {
        .name = "terminal.send",
        .description = "Types text verbatim into a session, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","properties":{"text":{"type":"string","minLength":1,"description":"The bytes to type; no newline is added."},"session":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A session ID; 0 or absent creates one."}},"required":["text"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"text":"q","session":3},"result":{"content":[{"type":"text","text":"sent"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_send",
    },
    MethodDescriptor {
        .name = "terminal.interrupt",
        .description = "Sends Control-C to a session, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":0,"maximum":4294967295,"description":"A session ID; 0 or absent creates one."}},"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"session":3},"result":{"content":[{"type":"text","text":"interrupted"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_interrupt",
    },
    MethodDescriptor {
        .name = "terminal.resize",
        .description = "Resizes a session's grid, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295},"columns":{"type":"integer","minimum":1,"maximum":1000},"rows":{"type":"integer","minimum":1,"maximum":1000}},"required":["session","columns","rows"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"session":3,"columns":100,"rows":30},"result":{"content":[{"type":"text","text":"resized"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_resize",
    },
    MethodDescriptor {
        .name = "terminal.wait",
        .description = "Waits until a session's screen shows a substring or the timeout passes, and returns the screen.",
        .parametersSchema = R"json({"type":"object","properties":{"substring":{"type":"string","minLength":1},"session":{"type":"integer","minimum":1,"maximum":4294967295},"timeout_ms":{"type":"integer","minimum":0,"maximum":2147483647,"default":10000}},"required":["substring","session"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"substring":"$ ","session":3},"result":{"content":[{"type":"text","text":"FOUND\n$ "}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_wait",
    },
    MethodDescriptor {
        .name = "terminal.wait_idle",
        .description = "Waits until a session's foreground returns to its shell, or the timeout passes.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295},"timeout_ms":{"type":"integer","minimum":0,"maximum":2147483647,"default":10000}},"required":["session"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"session":3},"result":{"content":[{"type":"text","text":"idle"}]}})json",
        .principals = agentCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_wait_idle",
    },
    MethodDescriptor {
        .name = "terminal.read_last",
        .description = "Returns the output of a session's last command, as its shell integration marked it.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295},"timeout_ms":{"type":"integer","minimum":0,"maximum":2147483647,"default":10000}},"required":["session"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"session":3},"result":{"content":[{"type":"text","text":"ok\n"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_read_last",
    },
    MethodDescriptor {
        .name = "terminal.list",
        .description = "Lists the daemon's sessions with their size, whether each is at its prompt, its foreground process, and its directory.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{},"result":{"content":[{"type":"text","text":"3\t80x24\tidle\tzsh\t/home/user\n"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_list",
    },
    MethodDescriptor {
        .name = "terminal.kill",
        .description = "Ends a session by hanging up its shell, once the agent policy and the session's owner allow it.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["session"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"session":3},"result":{"content":[{"type":"text","text":"terminated"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_kill",
    },
    MethodDescriptor {
        .name = "terminal.screenshot",
        .description = "Renders a session's screen to a PNG image, for when pixels matter more than text.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295},"path":{"type":"string","minLength":1,"description":"A bare file name to also save the image under in the screenshots directory."}},"required":["session"],"additionalProperties":false})json",
        .resultSchema = toolResultSchema,
        .example = R"json({"params":{"session":3},"result":{"content":[{"type":"image","data":"iVBORw0KGgo=","mimeType":"image/png"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "terminal_screenshot",
    },
    MethodDescriptor {
        .name = "ubq.ls",
        .description = "Lists the daemon's sessions with their session, window, tab and pane labels, size, prompt state, foreground process and directory.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sessions":{"type":"array","items":{"type":"object","properties":{"session":{"type":"string"},"label":{"type":"string"},"window":{"type":"string"},"windowLabel":{"type":"string"},"tab":{"type":"string"},"tabLabel":{"type":"string"},"pane":{"type":"string"},"paneLabel":{"type":"string"},"columns":{"type":"integer"},"rows":{"type":"integer"},"atPrompt":{"type":"boolean"},"foreground":{"type":"string"},"directory":{"type":"string"},"title":{"type":"string"}},"required":["session","pane","atPrompt"]}}},"required":["sessions"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"sessions":[{"session":"session:3","label":"build","window":"window:1","windowLabel":"workbench","tab":"tab:1","tabLabel":"tests","pane":"pane:2","paneLabel":"","columns":80,"rows":24,"atPrompt":true,"foreground":"zsh","directory":"/home/user","title":"zsh"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "ls",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.new",
        .description = "Creates a session running the login shell, optionally labels it and types a command into it, and returns its ID, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","properties":{"label":{"type":"string","maxLength":64,"description":"A label to address the session by; setting it needs the drive scope and a person's approval."},"command":{"type":"string","minLength":1,"description":"A command line typed into the new shell; typing it needs the drive scope."},"columns":{"type":"integer","minimum":1,"maximum":1000,"default":80},"rows":{"type":"integer","minimum":1,"maximum":1000,"default":24}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"session":{"type":"string"},"pane":{"type":"string"}},"required":["session","pane"],"additionalProperties":false})json",
        .example = R"json({"params":{"label":"build","command":"make"},"result":{"session":"session:3","pane":"pane:2"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "new",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Control, Sync::Scope::NewSession },
    },
    MethodDescriptor {
        .name = "ubq.run",
        .description = "Types a policy-approved command and waits for its marked start and finish. Without shell marks, returns a settled screen with outcome settled and exit code 11, without a command status. A timeout exits 9; --timeout bounds startup, input delivery and completion.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."},"command":{"type":"string","minLength":1,"description":"The command line to run."},"timeout":{"type":"number","minimum":0,"description":"Seconds to wait for the command to finish; absent waits as long as it runs."}},"required":["command"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"session":{"type":"string"},"exitStatus":{"type":"integer","description":"The command's status; absent when its shell reported none."},"output":{"type":"string"},"cursor":{"type":"integer"},"outcome":{"type":"string","enum":["settled"],"description":"An unmarked shell regained the foreground and its changed screen settled; no exit status is known."}},"required":["session","output","cursor"],"additionalProperties":false})json",
        .example = R"json({"params":{"session":"build","command":"make test"},"result":{"session":"session:3","exitStatus":0,"output":"ok\n","cursor":41}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "run",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::RegionRead },
    },
    MethodDescriptor {
        .name = "ubq.send",
        .description = "Types text verbatim into a session, and returns the event cursor from before it was typed, once the agent policy allows it.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."},"text":{"type":"string","minLength":1,"description":"The bytes to type."},"enter":{"type":"boolean","default":false,"description":"Whether to press Return after the text."}},"required":["text"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sent":{"type":"integer","minimum":0},"cursor":{"type":"integer","minimum":0}},"required":["sent","cursor"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":"logs","text":"q"},"result":{"sent":1,"cursor":40}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "send",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.read",
        .description = "Reads a session's screen, its last command's output, or the last lines of its scrollback as UTF-8 text.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."},"source":{"type":"string","enum":["screen","last-command","scrollback"],"default":"screen"},"lines":{"type":"integer","minimum":1,"maximum":100000,"default":100,"description":"For scrollback, how many lines ending at the bottom of the screen."}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"source":{"type":"string","enum":["screen","last-command","scrollback"]},"text":{"type":"string"},"screen":{"type":"object","description":"For screen reads, the complete pane.capture result including rich-text source and native layout geometry."}},"required":["source","text"],"additionalProperties":false})json",
        .example = R"json({"params":{"session":"build","source":"last-command"},"result":{"source":"last-command","text":"ok\n"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "read",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Control, Sync::Scope::RegionRead },
    },
    MethodDescriptor {
        .name = "pane.search",
        .description = "Searches a pane's whole history on the host by logical line, whatever of it the caller has fetched, and returns each match's line and cells.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The pane's handle on the caller's Sync connection; `ubq search` takes the pane from -s or -p and supplies its handle."},"pattern":{"type":"string","format":"utf8","minLength":1,"maxLength":127,"description":"A regular expression."},"from":{"type":"integer","minimum":0,"maximum":9007199254740991,"description":"The first line searched; the oldest the host keeps when absent."},"to":{"type":"integer","minimum":0,"maximum":9007199254740991,"description":"The line past the last searched; the history's head when absent."},"caseSensitive":{"type":"boolean","default":false}},"required":["pane","pattern"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"epoch":{"type":"integer","minimum":0},"matches":{"type":"array","items":{"type":"object","properties":{"line":{"type":"integer","minimum":0},"startCell":{"type":"integer","minimum":0},"endCell":{"type":"integer","minimum":0}},"required":["line","startCell","endCell"],"additionalProperties":false}},"truncated":{"type":"boolean"}},"required":["epoch","matches","truncated"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1,"pattern":"error: .*"},"result":{"epoch":1,"matches":[{"line":1204,"startCell":0,"endCell":17}],"truncated":false}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "search",
        .mcpTool = "",
        .syncMethod = "pane.search",
        .roles = anyAttachment,
        .scope = Sync::Scope::Observe,
        .target = CallTarget::Pane,
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "file.read",
        .description = "Reads at most 4 MiB from a regular file confined to the bound pane's kernel-reported foreground working directory. The result echoes the requested path and carries exact bytes as base64; large results use the call bulk lane. FileRead scope is independent of presentation role.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"path":{"type":"string","minLength":1,"maxLength":65535}},"required":["pane","path"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"path":{"type":"string","minLength":1,"maxLength":65535},"status":{"type":"string","enum":["ok","not-found","too-large","outside-confinement","not-regular","denied"]},"bytes":{"type":"integer","minimum":0,"maximum":4194304},"data":{"type":"string","maxLength":5592408}},"required":["pane","path","status","bytes","data"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1,"path":"/srv/readme"},"result":{"pane":1,"path":"/srv/readme","status":"ok","bytes":3,"data":"YWJj"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "file.read",
        .roles = anyAttachment,
        .scope = Sync::Scope::FileRead,
        .target = CallTarget::Pane,
    },
    MethodDescriptor {
        .name = "paste.push",
        .description = "Lands at most 4 MiB of a driver's pasted bytes as a private host file and types its path into the exact bound pane. A small manifest receives correlated PasteReady upload credit; verified raw chunks precede the one final result.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"mime":{"type":"string","maxLength":65535},"bytes":{"type":"integer","minimum":1,"maximum":4194304},"sha256":{"type":"string","minLength":64,"maxLength":64}},"required":["pane","mime","bytes","sha256"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"status":{"type":"string","enum":["landed","too-large","io-error","unsupported","busy"]},"path":{"type":"string","maxLength":65535}},"required":["pane","status","path"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1,"mime":"image/png","bytes":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},"result":{"pane":1,"status":"landed","path":"/tmp/ubiquitty-paste-example.png"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "paste.push",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Pane,
    },
    MethodDescriptor {
        .name = "pane.processes",
        .description = "Reads the bound pane's PTY process subtree, including background jobs, parent IDs, foreground membership and commands. Availability and completeness are explicit; at most 128 records and 48 KB of process JSON are returned.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["pane"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1},"available":{"type":"boolean"},"complete":{"type":"boolean"},"processes":{"type":"array","maxItems":128,"items":{"type":"object","properties":{"pid":{"type":"integer"},"parentPID":{"type":"integer"},"groupID":{"type":"integer"},"foreground":{"type":"boolean"},"leader":{"type":"boolean"},"name":{"type":"string","maxLength":256},"executable":{"type":"string","maxLength":256},"command":{"type":"string","maxLength":2048},"user":{"type":"string","maxLength":128},"state":{"type":"string","enum":["running","sleeping","stopped","zombie","unknown"]}},"required":["pid","parentPID","groupID","foreground","leader","name","executable","command","user","state"],"additionalProperties":false}}},"required":["pane","available","complete","processes"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1},"result":{"pane":1,"available":true,"complete":true,"processes":[]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.processes",
        .roles = anyAttachment,
        .scope = Sync::Scope::Observe,
        .target = CallTarget::Pane,
    },
    MethodDescriptor {
        .name = "pane.capture",
        .description = "Reads a pane's visible screen, last OSC-133 command output, inclusive retained row range, or tail on the host as UTF-8. Every returned row ends in a newline. Screen reports rows; other sources report source, first, last and totalRows. Complete is false if the text bound omitted rows. Screen text includes visible rich-text overlays. RichText carries full logical block text, visible scalar ranges, styles, and optional native geometry in points. Serial plus typographyEpoch identifies the layout; layoutPending asks the caller to retry. Selection belongs to a client, not this host call.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The pane's handle on the caller's Sync connection."},"source":{"type":"string","enum":["screen","last-command","rows","tail"],"default":"screen"},"start":{"type":"integer","minimum":-2147483648,"maximum":2147483647,"description":"Required only for rows; inclusive physical row, clamped to the retained buffer."},"end":{"type":"integer","minimum":-2147483648,"maximum":2147483647,"description":"Required only for rows; inclusive physical row, clamped to the retained buffer."},"lines":{"type":"integer","minimum":1,"maximum":100000,"description":"Required only for tail; includes visible rows."}},"required":["pane"],"additionalProperties":false})json",
        // Native geometry uses signed 32-bit 1/4096-point positions; cells use maxCellSize / 64 points.
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"rows":{"type":"integer","minimum":1,"maximum":1024},"columns":{"type":"integer","minimum":1,"maximum":1024},"source":{"type":"string","enum":["last-command","rows","tail"]},"first":{"type":"integer","minimum":0,"maximum":2147483647},"last":{"type":"integer","minimum":0,"maximum":2147483647},"totalRows":{"type":"integer","minimum":1,"maximum":2147483647},"complete":{"type":"boolean","description":"False when native layout is pending or the text size bound omitted rows."},"text":{"type":"string"},"richText":{"type":"object","properties":{"offsetUnit":{"type":"string","enum":["unicode-scalar"]},"coordinateUnit":{"type":"string","enum":["point"]},"layoutPending":{"type":"boolean"},"cellWidth":{"type":"number","minimum":0,"maximum":100},"cellHeight":{"type":"number","minimum":0,"maximum":100},"blocks":{"type":"array","maxItems":256,"items":{"type":"object","properties":{"id":{"type":"integer","minimum":0,"maximum":65535},"documentRevision":{"type":"integer","minimum":0,"maximum":4294967295},"serial":{"type":"integer","minimum":1,"maximum":4294967295},"typographyEpoch":{"type":"integer","minimum":0,"maximum":4294967295},"row":{"type":"integer","minimum":-1048576,"maximum":1024},"column":{"type":"integer","minimum":0,"maximum":1023},"width":{"type":"integer","minimum":1,"maximum":1024},"height":{"type":"integer","minimum":1,"maximum":1024},"sourceAnchor":{"type":"integer","minimum":0,"maximum":4096},"viewportTop":{"type":"integer","minimum":0,"maximum":4096},"visibleRects":{"type":"array","maxItems":32,"items":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":1023},"y":{"type":"integer","minimum":0,"maximum":1023},"width":{"type":"integer","minimum":1,"maximum":1024},"height":{"type":"integer","minimum":1,"maximum":1024}},"required":["x","y","width","height"],"additionalProperties":false}},"shaped":{"type":"boolean"},"text":{"type":"string","maxLength":16384},"visible":{"type":"array","maxItems":4096,"items":{"type":"object","properties":{"first":{"type":"integer","minimum":0,"maximum":4096},"end":{"type":"integer","minimum":0,"maximum":4096},"row":{"type":"integer","minimum":0,"maximum":1023},"column":{"type":"integer","minimum":0,"maximum":1023}},"required":["first","end","row","column"],"additionalProperties":false}},"lines":{"type":"array","maxItems":4096,"items":{"type":"object","properties":{"first":{"type":"integer","minimum":0,"maximum":4096},"end":{"type":"integer","minimum":0,"maximum":4096},"top":{"type":"number","minimum":0,"maximum":524287.999755859375},"height":{"type":"number","minimum":0,"maximum":524287.999755859375},"baseline":{"type":"number","minimum":0,"maximum":524287.999755859375},"x":{"type":"number","minimum":0,"maximum":524287.999755859375},"width":{"type":"number","minimum":0,"maximum":524287.999755859375}},"required":["first","end","top","height","baseline","x","width"],"additionalProperties":false}},"graphemes":{"type":"array","maxItems":4096,"items":{"type":"object","properties":{"first":{"type":"integer","minimum":0,"maximum":4096},"end":{"type":"integer","minimum":0,"maximum":4096},"line":{"type":"integer","minimum":0,"maximum":4095},"leading":{"type":"number","minimum":0,"maximum":524287.999755859375},"trailing":{"type":"number","minimum":0,"maximum":524287.999755859375}},"required":["first","end","line","leading","trailing"],"additionalProperties":false}},"runs":{"type":"array","maxItems":512,"items":{"type":"object","properties":{"first":{"type":"integer","minimum":0,"maximum":4096},"end":{"type":"integer","minimum":0,"maximum":4096},"halfPoints":{"type":"integer","minimum":1,"maximum":65535},"bold":{"type":"boolean"},"italic":{"type":"boolean"},"underline":{"type":"boolean"},"strikethrough":{"type":"boolean"},"family":{"type":"string","maxLength":256},"color":{"type":"integer","minimum":0,"maximum":15},"background":{"type":"integer","minimum":0,"maximum":15},"familyClass":{"type":"string","enum":["sans-serif","serif","monospace"]}},"required":["first","end","halfPoints","bold","italic","underline","strikethrough","family","color","background","familyClass"],"additionalProperties":false}},"colors":{"type":"array","maxItems":16,"items":{"type":"object","properties":{"red":{"type":"integer","minimum":0,"maximum":255},"green":{"type":"integer","minimum":0,"maximum":255},"blue":{"type":"integer","minimum":0,"maximum":255}},"required":["red","green","blue"],"additionalProperties":false}}},"required":["id","documentRevision","serial","typographyEpoch","row","column","width","height","sourceAnchor","viewportTop","visibleRects","shaped","text","visible","lines","graphemes","runs","colors"],"additionalProperties":false}}},"required":["offsetUnit","coordinateUnit","layoutPending","cellWidth","cellHeight","blocks"],"additionalProperties":false}},"required":["pane","columns","complete","text"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1},"result":{"pane":1,"rows":2,"columns":80,"complete":true,"text":"$ make\nok\n"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.capture",
        .roles = anyAttachment,
        .scope = Sync::Scope::RegionRead,
        .target = CallTarget::Pane,
    },
    MethodDescriptor {
        .name = "pane.pinGeometry",
        .description = "Freezes a pane's size whoever types, or with no size lets the newest typing driver's size apply again. Needs the Drive scope.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The pane's handle on the caller's Sync connection."},"rows":{"type":"integer","minimum":1,"maximum":1000},"columns":{"type":"integer","minimum":1,"maximum":1000}},"required":["pane"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pinned":{"type":"boolean"}},"required":["pinned"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1,"rows":30,"columns":100},"result":{"pinned":true}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.pinGeometry",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Pane,
    },
    MethodDescriptor {
        .name = "client.clipboardServing",
        .description = "Opts this connection's own clipboard into remote reads, or revokes it immediately. It changes no other client's routing and grants no access to another clipboard.",
        .parametersSchema = R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"],"additionalProperties":false})json",
        .example = R"json({"params":{"enabled":true},"result":{"enabled":true}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "client.clipboardServing",
        .roles = anyAttachment,
        .scope = Sync::Scope::Observe,
        .target = CallTarget::Connection,
    },
    MethodDescriptor {
        .name = "events.subscribe",
        .description = "Subscribes this connection to the retained event log after an exact saved cursor, or from the current position. Only records its existing attachment or standing grant admits are disclosed, except for the verified local person. A bounded replay reports retention gaps and never advances past an unadmitted notification.",
        .parametersSchema = R"json({"type":"object","properties":{"after":{"type":"string","minLength":1,"maxLength":20},"fromLatest":{"type":"boolean"}},"required":["after","fromLatest"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"cursor":{"type":"string","minLength":1,"maxLength":20}},"required":["cursor"],"additionalProperties":false})json",
        .example = R"json({"params":{"after":"0","fromLatest":true},"result":{"cursor":"7"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "events.subscribe",
        .roles = anyAttachment,
        .scope = Sync::Scope::Control,
        .target = CallTarget::Connection,
    },
    MethodDescriptor {
        .name = "host.refresh",
        .description = "Fetches retained notifications, unresolved questions and the latest pane records without attaching to a pane. A screen checkpoint additionally requires RegionRead. The pane continuation is opaque; zero starts or finishes a sweep.",
        .parametersSchema = R"json({"type":"object","properties":{"epoch":{"type":"string","maxLength":20},"inventory":{"type":"string","maxLength":20},"after":{"type":"string","maxLength":20},"paneOffset":{"type":"integer","minimum":0,"maximum":100001},"screenPane":{"type":"string","minLength":32,"maxLength":32},"screenRevision":{"type":"string","minLength":32,"maxLength":32}},"required":[],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"epoch":{"type":"string","maxLength":20},"inventory":{"type":"string","maxLength":20},"cursor":{"type":"string","maxLength":20},"hasMore":{"type":"boolean"},"gap":{"type":"boolean"},"hostNowUnixMilliseconds":{"type":"string","maxLength":20},"events":{"type":"array","items":{"type":"object","properties":{"sequence":{"type":"string","maxLength":20},"createdUnixMilliseconds":{"type":"string","maxLength":20},"paneID":{"type":"string","maxLength":32},"sessionID":{"type":"integer","minimum":0,"maximum":4294967295},"kind":{"type":"string","enum":["notification","question"]},"title":{"type":"string","maxLength":192},"body":{"type":"string","maxLength":2048},"statusID":{"type":"string","maxLength":128},"notificationID":{"type":"string","maxLength":32},"occasion":{"type":"string","enum":["always","unfocused","invisible"]}},"required":["sequence","createdUnixMilliseconds","paneID","sessionID","kind","title","body","statusID","notificationID","occasion"],"additionalProperties":false}},"panes":{"type":"array","items":{"type":"object","properties":{"paneID":{"type":"string","maxLength":32},"sessionID":{"type":"integer","minimum":0,"maximum":4294967295},"facts":{"type":"string"},"status":{"type":"array","items":{"type":"string"}}},"required":["paneID","sessionID","facts","status"],"additionalProperties":false}},"nextPaneOffset":{"type":"integer","minimum":0,"maximum":100001},"panesOmitted":{"type":"boolean"},"screen":{"type":"object","properties":{"paneID":{"type":"string","maxLength":32},"revision":{"type":"string","maxLength":32},"checkpoint":{"type":"string"}},"required":["paneID","revision","checkpoint"],"additionalProperties":false}},"required":["epoch","inventory","cursor","hasMore","gap","hostNowUnixMilliseconds","events","panes","nextPaneOffset","panesOmitted"],"additionalProperties":false})json",
        .example = R"json({"params":{"epoch":"1","after":"0","paneOffset":0},"result":{"epoch":"1","inventory":"1","cursor":"0","hasMore":false,"gap":false,"hostNowUnixMilliseconds":"1700000000000","events":[],"panes":[],"nextPaneOffset":0,"panesOmitted":false}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.refresh",
        .roles = { Role::Owner, Role::Driver, Role::Viewer },
        .scope = Sync::Scope::Observe,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.describe",
        .description = "Describes the host a Sync connection reached: the wire versions it speaks, the version of the program serving them, and the scopes the caller's credential carries there.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"wireVersion":{"type":"integer","minimum":1,"maximum":65535},"appVersion":{"type":"string"},"scopes":{"type":"array","items":{"type":"string","enum":["observe","drive","control","region","new","file","manage"]}}},"required":["wireVersion","appVersion","scopes"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"wireVersion":1,"appVersion":"1.0","scopes":["observe","drive","control"]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.describe",
        .roles = anyAttachment,
        .scope = Sync::Scope::Control,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.methods",
        .description = "Lists the methods the caller may call on this Sync connection, each with what it is addressed to and the scope it needs.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"methods":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"description":{"type":"string"},"target":{"type":"string","enum":["pane","host","connection"]},"scope":{"type":"string","enum":["observe","drive","control","region","new","file","manage"]}},"required":["name","description","target","scope"],"additionalProperties":false}}},"required":["methods"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"methods":[{"name":"pane.capture","description":"Reads a pane's visible screen.","target":"pane","scope":"region"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.methods",
        .roles = anyAttachment,
        .scope = Sync::Scope::Control,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.management.list",
        .description = "Lists admitted clients, grants and token metadata under Manage authority. Sessions are supplied by the host listing tree.",
        .parametersSchema = R"json({"type":"object","properties":{"daemonInstance":{"type":"string","minLength":1,"maxLength":20}},"required":["daemonInstance"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"method":{"type":"string","enum":["host.management.list"]},"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"status":{"type":"string","enum":["success","refused","not-found","invalid","failed"]},"detail":{"type":"string","maxLength":4096},"clients":{"type":"array","maxItems":1024,"items":{"type":"object","properties":{"client":{"type":"integer","minimum":0,"maximum":4294967295},"session":{"type":"integer","minimum":0,"maximum":4294967295},"attachedUnix":{"type":"string","minLength":1,"maxLength":20},"principal":{"type":"string","maxLength":4096},"label":{"type":"string","maxLength":4096},"transport":{"type":"string","maxLength":4096},"identity":{"type":"string","maxLength":4096}},"required":["client","session","attachedUnix","principal","label","transport","identity"],"additionalProperties":false}},"grants":{"type":"array","maxItems":1024,"items":{"type":"object","properties":{"identifier":{"type":"string","maxLength":4096},"capability":{"type":"string","maxLength":4096},"origin":{"type":"string","maxLength":4096},"identity":{"type":"string","maxLength":4096},"lifetime":{"type":"string","maxLength":4096},"session":{"type":"integer","minimum":0,"maximum":4294967295},"grantedUnix":{"type":"string","minLength":1,"maxLength":20},"grantedBy":{"type":"string","maxLength":4096}},"required":["identifier","capability","origin","identity","lifetime","session","grantedUnix","grantedBy"],"additionalProperties":false}},"tokens":{"type":"array","maxItems":1024,"items":{"type":"object","properties":{"fingerprint":{"type":"string","maxLength":4096},"label":{"type":"string","maxLength":4096},"scopes":{"type":"integer","minimum":0,"maximum":127},"expiryUnix":{"type":"string","minLength":1,"maxLength":20},"createdUnix":{"type":"string","minLength":1,"maxLength":20},"lastUsedUnix":{"type":"string","minLength":1,"maxLength":20},"lastUsedBy":{"type":"string","maxLength":4096}},"required":["fingerprint","label","scopes","expiryUnix","createdUnix","lastUsedUnix","lastUsedBy"],"additionalProperties":false}}},"required":["method","daemonInstance","status","detail","clients","grants","tokens"],"additionalProperties":false})json",
        .example = R"json({"params":{"daemonInstance":"1"},"result":{"method":"host.management.list","daemonInstance":"1","status":"success","detail":"","clients":[],"grants":[],"tokens":[]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.management.list",
        .roles = anyAttachment,
        .scope = Sync::Scope::Manage,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.client.disconnect",
        .description = "Disconnects one admitted client other than the calling management connection.",
        .parametersSchema = R"json({"type":"object","properties":{"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"client":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["daemonInstance","client"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"method":{"type":"string","enum":["host.client.disconnect"]},"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"status":{"type":"string","enum":["success","refused","not-found","invalid","failed"]},"detail":{"type":"string","maxLength":4096},"client":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["method","daemonInstance","status","detail","client"],"additionalProperties":false})json",
        .example = R"json({"params":{"daemonInstance":"1","client":13},"result":{"method":"host.client.disconnect","daemonInstance":"1","status":"success","detail":"","client":13}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.client.disconnect",
        .roles = anyAttachment,
        .scope = Sync::Scope::Manage,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.grant.revoke",
        .description = "Revokes one exact host-owned capability, attachment or session grant.",
        .parametersSchema = R"json({"type":"object","properties":{"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"identity":{"type":"string","minLength":1,"maxLength":4096}},"required":["daemonInstance","identity"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"method":{"type":"string","enum":["host.grant.revoke"]},"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"status":{"type":"string","enum":["success","refused","not-found","invalid","failed"]},"detail":{"type":"string","maxLength":4096},"identity":{"type":"string","minLength":1,"maxLength":4096}},"required":["method","daemonInstance","status","detail","identity"],"additionalProperties":false})json",
        .example = R"json({"params":{"daemonInstance":"1","identity":"attach:test-principal"},"result":{"method":"host.grant.revoke","daemonInstance":"1","status":"success","detail":"","identity":"attach:test-principal"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.grant.revoke",
        .roles = anyAttachment,
        .scope = Sync::Scope::Manage,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.token.issue",
        .description = "Issues one explicitly scoped expiring token and returns its bearer secret only on success.",
        .parametersSchema = R"json({"type":"object","properties":{"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"label":{"type":"string","minLength":1,"maxLength":128},"scopes":{"type":"integer","minimum":1,"maximum":127},"expiryUnix":{"type":"string","minLength":1,"maxLength":20}},"required":["daemonInstance","label","scopes","expiryUnix"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"method":{"type":"string","enum":["host.token.issue"]},"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"status":{"type":"string","enum":["success","refused","not-found","invalid","failed"]},"detail":{"type":"string","maxLength":4096},"label":{"type":"string","minLength":1,"maxLength":128},"scopes":{"type":"integer","minimum":1,"maximum":127},"expiryUnix":{"type":"string","minLength":1,"maxLength":20},"issuedToken":{"type":"string","minLength":1,"maxLength":4096}},"required":["method","daemonInstance","status","detail","label","scopes","expiryUnix"],"additionalProperties":false})json",
        .example = R"json({"params":{"daemonInstance":"1","label":"Example reader","scopes":1,"expiryUnix":"2000000000"},"result":{"method":"host.token.issue","daemonInstance":"1","status":"success","detail":"","label":"Example reader","scopes":1,"expiryUnix":"2000000000","issuedToken":"example-only-not-a-real-token"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.token.issue",
        .roles = anyAttachment,
        .scope = Sync::Scope::Manage,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.token.revoke",
        .description = "Revokes one exact token fingerprint and rechecks affected clients.",
        .parametersSchema = R"json({"type":"object","properties":{"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"identity":{"type":"string","minLength":1,"maxLength":4096}},"required":["daemonInstance","identity"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"method":{"type":"string","enum":["host.token.revoke"]},"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"status":{"type":"string","enum":["success","refused","not-found","invalid","failed"]},"detail":{"type":"string","maxLength":4096},"identity":{"type":"string","minLength":1,"maxLength":4096}},"required":["method","daemonInstance","status","detail","identity"],"additionalProperties":false})json",
        .example = R"json({"params":{"daemonInstance":"1","identity":"example-token-fingerprint"},"result":{"method":"host.token.revoke","daemonInstance":"1","status":"success","detail":"","identity":"example-token-fingerprint"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.token.revoke",
        .roles = anyAttachment,
        .scope = Sync::Scope::Manage,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.clear",
        .description = "Runs a clear command on the host-owned pane under Drive authority. Every attached client receives the resulting screen and any history reset.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"command":{"type":"string","enum":["buffer","scrollback","screen","previous-mark"]}},"required":["pane","command"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1},"command":{"type":"string","enum":["buffer","scrollback","screen","previous-mark"]}},"required":["pane","command"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1,"command":"buffer"},"result":{"pane":1,"command":"buffer"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.clear",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Pane,
    },
    MethodDescriptor {
        .name = "pane.reclaim",
        .description = "Releases an adopted pane to the attached person's retained local PTY master. Only the exact live attachment on a same-user local descriptor stream may request it.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"daemonInstance":{"type":"string","minLength":1,"maxLength":20}},"required":["pane","daemonInstance"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"session":{"type":"integer","minimum":1,"maximum":4294967295},"daemonInstance":{"type":"string","minLength":1,"maxLength":20},"host":{"type":"string","minLength":32,"maxLength":32},"attachment":{"type":"string","minLength":32,"maxLength":32},"paneIdentity":{"type":"string","minLength":32,"maxLength":32},"outcome":{"type":"string","enum":["released"]}},"required":["pane","session","daemonInstance","host","attachment","paneIdentity","outcome"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1,"daemonInstance":"1"},"result":{"pane":1,"session":2,"daemonInstance":"1","host":"00000000000000010000000000000001","attachment":"00000000000000020000000000000002","paneIdentity":"00000000000000030000000000000003","outcome":"released"}})json",
        .principals = Principals { Principal::Person },
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.reclaim",
        .roles = humanAttachments,
        .scope = Sync::Scope::Observe,
        .target = CallTarget::Pane,
        .origin = CallOrigin::Local,
    },
    MethodDescriptor {
        .name = "pane.overrideGeometry",
        .description = "Sets a pane's size once, as an agent's explicit resize: it holds until a driver types or reports a different size. Needs the Drive scope.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The pane's handle on the caller's Sync connection."},"rows":{"type":"integer","minimum":1,"maximum":1000},"columns":{"type":"integer","minimum":1,"maximum":1000}},"required":["pane","rows","columns"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"rows":{"type":"integer"},"columns":{"type":"integer"}},"required":["rows","columns"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":1,"rows":40,"columns":100},"result":{"rows":40,"columns":100}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.overrideGeometry",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Pane,
    },
    MethodDescriptor {
        .name = "session.terminate",
        .description = "Ends the exact live session under Control authority, or lets a person forget a remembered workspace. An unattached agent without an existing grant waits for one human decision; the result names the target and outcome, and silence is not success.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["session"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295},"outcome":{"type":"string","enum":["terminated","forgotten","denied","timed-out","refused"]}},"required":["session","outcome"],"additionalProperties":false})json",
        .example = R"json({"params":{"session":7},"result":{"session":7,"outcome":"terminated"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "session.terminate",
        .roles = anyAttachment,
        .scope = Sync::Scope::Control,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.session.terminate",
        .description = "Ends one exact live session using human Manage authority. It does not grant Drive, accept an agent, or infer success from silence.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["session"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"session":{"type":"integer","minimum":1,"maximum":4294967295},"outcome":{"type":"string","enum":["terminated","refused"]}},"required":["session","outcome"],"additionalProperties":false})json",
        .example = R"json({"params":{"session":7},"result":{"session":7,"outcome":"terminated"}})json",
        .principals = Principals { Principal::Person },
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.session.terminate",
        .roles = Roles { Role::Owner, Role::Driver, Role::Viewer },
        .scope = Sync::Scope::Manage,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.sessions.subscribe",
        .description = "Begins listing the host on this connection: the caller is sent every workspace's structure as it changes, and the facts and status of every pane it has not bound, at most once a second a pane. Answers with the exact structure and pane-facts revisions to await, the listed-pane count and the host's clock. The structure, names, labels, titles, statuses, directories and activity are served under this scope alone; a pane's command line with its arguments is sent only to a caller whose credential also carries Observe, which is what reading that pane's screen asks, and otherwise the facts carry the names of what runs without arguments.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"workspaces":{"type":"array","items":{"type":"integer","minimum":1}},"structureRevisions":{"type":"array","items":{"type":"string"}},"paneRevisions":{"type":"array","items":{"type":"object","properties":{"session":{"type":"integer","minimum":1},"revision":{"type":"integer","minimum":1}},"required":["session","revision"],"additionalProperties":false}},"panes":{"type":"integer","minimum":0},"hostNowUnixMilliseconds":{"type":"integer","minimum":0},"instances":{"type":"array","maxItems":32,"items":{"type":"object","properties":{"socket":{"type":"string","maxLength":26},"executable":{"type":"string","maxLength":4096},"startedUnixSeconds":{"type":"integer","minimum":0}},"required":["socket","executable","startedUnixSeconds"],"additionalProperties":false}}},"required":["workspaces","structureRevisions","paneRevisions","panes","hostNowUnixMilliseconds"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"workspaces":[1,4],"structureRevisions":["2","7"],"paneRevisions":[{"session":1,"revision":2}],"panes":1,"hostNowUnixMilliseconds":1700000000000}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.sessions.subscribe",
        .roles = anyAttachment,
        .scope = Sync::Scope::Control,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "host.sessions.unsubscribe",
        .description = "Ends the listing this connection began: the panes it was listed are unlisted and no workspace's structure is owed it but those of the workspaces it is attached to.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","additionalProperties":false})json",
        .example = R"json({"params":{},"result":{}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "host.sessions.unsubscribe",
        .roles = anyAttachment,
        .scope = Sync::Scope::Control,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "client.label",
        .description = "Labels only the sending live client, or clears its label with an empty value. Requires Drive and the authority to rearrange its attached workspace; an agent needs standing for every session and no person may be attached. It cannot name another client.",
        .parametersSchema = R"json({"type":"object","properties":{"label":{"type":"string","maxLength":1024,"description":"An optional label of at most 64 bytes, excluding control characters, reserved words and identities."}},"required":["label"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"client":{"type":"string"},"label":{"type":"string"}},"required":["client","label"],"additionalProperties":false})json",
        .example = R"json({"params":{"label":"laptop"},"result":{"client":"client:7","label":"laptop"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "client.label",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "label.set",
        .description = "Sets the label of a workspace, a window, a tab, a pane or a session, or clears it with an empty one. A label the host does not take is refused with the reason, and a change reaches every listing of the host.",
        .parametersSchema = R"json({"type":"object","properties":{"object":{"type":"string","minLength":1,"maxLength":40,"description":"A workspace, window, tab, pane or session of the host, written kind:number as its listing names it: workspace:3, pane:12."},"label":{"type":"string","maxLength":1024,"description":"The label; empty clears it."}},"required":["object","label"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"object":{"type":"string"},"label":{"type":"string"}},"required":["object","label"],"additionalProperties":false})json",
        .example = R"json({"params":{"object":"tab:7","label":"logs"},"result":{"object":"tab:7","label":"logs"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "label.set",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "tags.set",
        .description = "Replaces the tags a person put on a workspace, a window, a tab, a pane or a session, or clears them with none. A name given twice is kept once; tags the host does not take are refused with the reason and change nothing.",
        .parametersSchema = R"json({"type":"object","properties":{"object":{"type":"string","minLength":1,"maxLength":40,"description":"A workspace, window, tab, pane or session of the host, written kind:number as its listing names it: workspace:3, pane:12."},"tags":{"type":"array","items":{"type":"string","maxLength":1024},"description":"The tags, in the order they are drawn; none clears them."}},"required":["object","tags"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"object":{"type":"string"},"tags":{"type":"array","items":{"type":"string"}}},"required":["object","tags"],"additionalProperties":false})json",
        .example = R"json({"params":{"object":"window:2","tags":["production","billing"]},"result":{"object":"window:2","tags":["production","billing"]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "tags.set",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "workspace.share",
        .description = "Places already adopted local terminals in one window of a new or existing workspace. Only a trusted local person may publish the arrangement; this starts no programs.",
        .parametersSchema = R"json({"type":"object","properties":{"daemonInstanceID":{"type":"string","minLength":1,"maxLength":20},"workspace":{"type":"integer","minimum":0,"maximum":4294967295},"arrangement":{"type":"string","minLength":1,"maxLength":60000},"sessions":{"type":"array","minItems":1,"maxItems":256,"items":{"type":"integer","minimum":1,"maximum":4294967295}}},"required":["daemonInstanceID","workspace","arrangement","sessions"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"workspace":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["workspace"],"additionalProperties":false})json",
        .example = R"json({"params":{"daemonInstanceID":"1","workspace":0,"arrangement":"encoded arrangement","sessions":[1]},"result":{"workspace":1}})json",
        .principals = Principals { Principal::Person },
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "workspace.share",
        .roles = Roles { Role::Owner, Role::Driver },
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "workspace.rename",
        .description = "Gives a workspace the name it is reached and recorded by, or takes its name away with an empty one. A name another workspace holds, or one longer than a name may be, is refused with the reason.",
        .parametersSchema = R"json({"type":"object","properties":{"workspace":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The workspace's number."},"name":{"type":"string","maxLength":4096}},"required":["workspace","name"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"workspace":{"type":"string"},"name":{"type":"string"}},"required":["workspace","name"],"additionalProperties":false})json",
        .example = R"json({"params":{"workspace":3,"name":"client work"},"result":{"workspace":"workspace:3","name":"client work"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "workspace.rename",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "tab.rename",
        .description = "Gives a tab its name, or takes it away with an empty one. A name longer than a name may be is refused with the reason.",
        .parametersSchema = R"json({"type":"object","properties":{"tab":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The tab's number."},"name":{"type":"string","maxLength":4096}},"required":["tab","name"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"tab":{"type":"string"},"name":{"type":"string"}},"required":["tab","name"],"additionalProperties":false})json",
        .example = R"json({"params":{"tab":7,"name":"logs"},"result":{"tab":"tab:7","name":"logs"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "tab.rename",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "tab.move",
        .description = "Moves a tab to a place among the tabs of a window of its workspace: within its own window it is reordered, into another it arrives shown, and a window left with no tab is closed. The tab keeps its identity, name, label and panes.",
        .parametersSchema = R"json({"type":"object","properties":{"tab":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The tab's number."},"window":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The window it is to stand in, of the same workspace."},"index":{"type":"integer","minimum":0,"maximum":32,"description":"The place it takes there, from zero, counted once it has left where it stood."}},"required":["tab","window","index"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"tab":{"type":"string"},"window":{"type":"string"},"index":{"type":"integer","minimum":0}},"required":["tab","window","index"],"additionalProperties":false})json",
        .example = R"json({"params":{"tab":7,"window":2,"index":0},"result":{"tab":"tab:7","window":"window:2","index":0}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "tab.move",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.moveToTab",
        .description = "Moves an existing pane into a named tab beside a target pane or its complete layout. Source and destination must share one workspace at the exact observed revision; no session is created.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"tab":{"type":"integer","minimum":1,"maximum":4294967295},"target":{"type":"integer","minimum":1,"maximum":4294967295},"orientation":{"type":"string","enum":["horizontal","vertical"]},"side":{"type":"string","enum":["before","after"]},"insertTarget":{"type":"string","enum":["pane","root"]},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","pane","tab","target","orientation","side","insertTarget","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sourceWindow":{"type":"string"},"sourceTab":{"type":"string"},"pane":{"type":"string"},"window":{"type":"string"},"tab":{"type":"string"},"target":{"type":"string"},"orientation":{"type":"string"},"side":{"type":"string"},"insertTarget":{"type":"string"}},"required":["sourceWindow","sourceTab","pane","window","tab","target","orientation","side","insertTarget"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"pane":2,"tab":3,"target":4,"orientation":"vertical","side":"before","insertTarget":"root","expectedRevision":"7"},"result":{"sourceWindow":"window:1","sourceTab":"tab:1","pane":"pane:2","window":"window:2","tab":"tab:3","target":"pane:4","orientation":"vertical","side":"before","insertTarget":"root"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.moveToTab",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "tab.moveToPane",
        .description = "Merges an existing tab into the target pane's tab beside the target pane or its complete layout. Source and destination must share one workspace at the exact observed revision; no session is created.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"tab":{"type":"integer","minimum":1,"maximum":4294967295},"target":{"type":"integer","minimum":1,"maximum":4294967295},"orientation":{"type":"string","enum":["horizontal","vertical"]},"side":{"type":"string","enum":["before","after"]},"insertTarget":{"type":"string","enum":["pane","root"]},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","tab","target","orientation","side","insertTarget","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sourceWindow":{"type":"string"},"sourceTab":{"type":"string"},"window":{"type":"string"},"tab":{"type":"string"},"target":{"type":"string"},"orientation":{"type":"string"},"side":{"type":"string"},"insertTarget":{"type":"string"}},"required":["sourceWindow","sourceTab","window","tab","target","orientation","side","insertTarget"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"tab":2,"target":4,"orientation":"vertical","side":"before","insertTarget":"root","expectedRevision":"7"},"result":{"sourceWindow":"window:1","sourceTab":"tab:2","window":"window:2","tab":"tab:3","target":"pane:4","orientation":"vertical","side":"before","insertTarget":"root"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "tab.moveToPane",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.moveToNewTab",
        .description = "Moves an existing pane to a separate tab in the named destination window, reusing its tab when it is already the only pane. Source and destination must share one workspace at the exact observed revision; no session is created.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"targetWindow":{"type":"integer","minimum":1,"maximum":4294967295},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","pane","targetWindow","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sourceWindow":{"type":"string"},"sourceTab":{"type":"string"},"pane":{"type":"string"},"window":{"type":"string"},"tab":{"type":"string"}},"required":["sourceWindow","sourceTab","pane","window","tab"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"pane":2,"targetWindow":3,"expectedRevision":"7"},"result":{"sourceWindow":"window:1","sourceTab":"tab:1","pane":"pane:2","window":"window:3","tab":"tab:4"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.moveToNewTab",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.extractTab",
        .description = "Moves an existing pane into a new tab in its current window without creating a session. The source window and exact observed structure revision must still match.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","pane","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sourceWindow":{"type":"string"},"sourceTab":{"type":"string"},"pane":{"type":"string"},"window":{"type":"string"},"tab":{"type":"string"}},"required":["sourceWindow","sourceTab","pane","window","tab"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"pane":3,"expectedRevision":"7"},"result":{"sourceWindow":"window:1","sourceTab":"tab:2","pane":"pane:3","window":"window:1","tab":"tab:4"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.extractTab",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.extractWindow",
        .description = "Moves an existing pane into a new window and tab without creating a session. The source window and exact observed structure revision must still match.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","pane","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sourceWindow":{"type":"string"},"sourceTab":{"type":"string"},"pane":{"type":"string"},"window":{"type":"string"},"tab":{"type":"string"}},"required":["sourceWindow","sourceTab","pane","window","tab"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"pane":3,"expectedRevision":"7"},"result":{"sourceWindow":"window:1","sourceTab":"tab:2","pane":"pane:3","window":"window:4","tab":"tab:5"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.extractWindow",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "tab.extractWindow",
        .description = "Moves an existing tab into a new window without changing its pane or session identities. The source window and exact observed structure revision must still match.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"tab":{"type":"integer","minimum":1,"maximum":4294967295},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","tab","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sourceWindow":{"type":"string"},"tab":{"type":"string"},"window":{"type":"string"}},"required":["sourceWindow","tab","window"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"tab":2,"expectedRevision":"7"},"result":{"sourceWindow":"window:1","tab":"tab:2","window":"window:3"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "tab.extractWindow",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.move",
        .description = "Moves a pane beside another pane in the same window's active tab, preserving both sessions. The exact observed structure revision must still be current.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"target":{"type":"integer","minimum":1,"maximum":4294967295},"orientation":{"type":"string","enum":["horizontal","vertical"]},"side":{"type":"string","enum":["before","after"]},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","pane","target","orientation","side","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"string"},"pane":{"type":"string"},"target":{"type":"string"},"orientation":{"type":"string"},"side":{"type":"string"}},"required":["window","pane","target","orientation","side"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"pane":2,"target":3,"orientation":"vertical","side":"before","expectedRevision":"7"},"result":{"window":"window:1","pane":"pane:2","target":"pane:3","orientation":"vertical","side":"before"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.move",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.swap",
        .description = "Exchanges two panes in the same window's active tab, preserving their sessions. The exact observed structure revision must still be current.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"target":{"type":"integer","minimum":1,"maximum":4294967295},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","pane","target","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"string"},"pane":{"type":"string"},"target":{"type":"string"}},"required":["window","pane","target"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"pane":2,"target":3,"expectedRevision":"7"},"result":{"window":"window:1","pane":"pane:2","target":"pane:3"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.swap",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "window.activate",
        .description = "Shows a named window as the workspace's active window. An agent needs approval of this exact navigation.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["window"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"string"}},"required":["window"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":2},"result":{"window":"window:2"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "window.activate",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "tab.activate",
        .description = "Shows a named tab in its existing window. An agent needs approval of this exact navigation.",
        .parametersSchema = R"json({"type":"object","properties":{"tab":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["tab"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"tab":{"type":"string"},"window":{"type":"string"}},"required":["tab","window"],"additionalProperties":false})json",
        .example = R"json({"params":{"tab":7},"result":{"tab":"tab:7","window":"window:2"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "tab.activate",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.focus",
        .description = "Focuses a named pane in its window's active tab. An agent needs approval of this exact navigation.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["pane"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"string"},"window":{"type":"string"}},"required":["pane","window"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":3},"result":{"pane":"pane:3","window":"window:2"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.focus",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "window.moveFocus",
        .description = "Moves focus in a named direction from the exact observed pane and structure revision. Stale intent is refused; callers refresh without automatically retrying.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"direction":{"type":"string","enum":["left","right","up","down"]},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","pane","direction","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"string"},"fromPane":{"type":"string"},"pane":{"type":"string"},"direction":{"type":"string","enum":["left","right","up","down"]}},"required":["window","fromPane","pane","direction"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":2,"pane":3,"direction":"right","expectedRevision":"7"},"result":{"window":"window:2","fromPane":"pane:3","pane":"pane:4","direction":"right"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "window.moveFocus",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.zoom",
        .description = "Sets a pane's zoom state at the exact observed workspace structure revision. Stale intent is refused; callers refresh without automatically retrying.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"state":{"type":"string","enum":["zoomed","unzoomed"]},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["pane","state","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"string"},"state":{"type":"string","enum":["zoomed","unzoomed"]}},"required":["pane","state"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":3,"state":"zoomed","expectedRevision":"7"},"result":{"pane":"pane:3","state":"zoomed"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.zoom",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "window.close",
        .description = "Closes a window only at the exact workspace structure revision confirmed by the caller, ending every session in its panes. An agent requires an attached person's approval. Closing the last window ends its workspace.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295,"description":"The window's number."},"expectedRevision":{"type":"string","minLength":1,"maxLength":20,"description":"The nonzero workspace structure revision from the current host tree, as an exact decimal uint64 string."}},"required":["window","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"string"},"sessionsEnded":{"type":"integer","minimum":0}},"required":["window","sessionsEnded"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":2,"expectedRevision":"7"},"result":{"window":"window:2","sessionsEnded":3}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "window.close",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "tab.balance",
        .description = "Balances the named tab at its exact workspace revision without changing selection. An agent needs approval of this exact change.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"tab":{"type":"integer","minimum":1,"maximum":4294967295},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","tab","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"string"},"tab":{"type":"string"}},"required":["window","tab"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"tab":2,"expectedRevision":"7"},"result":{"window":"window:1","tab":"tab:2"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "tab.balance",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "pane.adjustRatio",
        .description = "Moves the named pane's divider by a positive step in its explicit tab at the exact workspace revision. Selection is unchanged. An agent needs approval of this exact change.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"tab":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"direction":{"type":"string","enum":["left","right","up","down"]},"step":{"type":"number","minimum":0,"maximum":1,"description":"Must be strictly positive."},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","tab","pane","direction","step","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"string"},"tab":{"type":"string"},"pane":{"type":"string"},"direction":{"type":"string"},"step":{"type":"number"}},"required":["window","tab","pane","direction","step"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"tab":2,"pane":3,"direction":"left","step":0.1,"expectedRevision":"7"},"result":{"window":"window:1","tab":"tab:2","pane":"pane:3","direction":"left","step":0.1}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "pane.adjustRatio",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "divider.setRatio",
        .description = "Sets a split's fraction in the exact named tab revision. A binary child path selects the split, with an empty path selecting the root; it is not a stable divider identity. Selection is unchanged. An agent needs approval of this exact change.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"tab":{"type":"integer","minimum":1,"maximum":4294967295},"path":{"type":"array","maxItems":63,"items":{"type":"integer","minimum":0,"maximum":1}},"ratio":{"type":"number","minimum":0,"maximum":1},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","tab","path","ratio","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"window":{"type":"string"},"tab":{"type":"string"},"path":{"type":"array","maxItems":63,"items":{"type":"integer","minimum":0,"maximum":1}},"ratio":{"type":"number"}},"required":["window","tab","path","ratio"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":1,"tab":2,"path":[1],"ratio":0.4,"expectedRevision":"7"},"result":{"window":"window:1","tab":"tab:2","path":[1],"ratio":0.4}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "divider.setRatio",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "workspace.addWindow",
        .description = "Creates a window, tab and session from the named source pane at an exact workspace revision. The requester retains Drive and NewSession authority; an agent needs an attached person's approval of this exact change.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295},"expectedRevision":{"type":"string","minLength":1,"maxLength":20}},"required":["window","pane","expectedRevision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"sourceWindow":{"type":"string"},"sourcePane":{"type":"string"},"window":{"type":"string"},"tab":{"type":"string"},"pane":{"type":"string"},"session":{"type":"string"}},"required":["sourceWindow","sourcePane","window","tab","pane","session"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":2,"pane":3,"expectedRevision":"7"},"result":{"sourceWindow":"window:2","sourcePane":"pane:3","window":"window:4","tab":"tab:7","pane":"pane:8","session":"session:9"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "workspace.addWindow",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "workspace.addTab",
        .description = "Adds a tab with a new session to a window. An agent waits for an attached person to approve this exact change, and the requester must retain Drive and NewSession scopes.",
        .parametersSchema = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":1,"maximum":4294967295},"pane":{"type":"integer","minimum":1,"maximum":4294967295,"description":"Optional pane in the named window whose theme the new session inherits."}},"required":["window"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"tab":{"type":"string"},"pane":{"type":"string"},"session":{"type":"string"}},"required":["tab","pane","session"],"additionalProperties":false})json",
        .example = R"json({"params":{"window":2},"result":{"tab":"tab:7","pane":"pane:8","session":"session:9"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "workspace.addTab",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "workspace.split",
        .description = "Splits a named pane in its window's active tab with a new session, using a vertical or horizontal divider. An agent needs exact-operation approval from an attached person; the requester retains Drive and NewSession authority.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295},"orientation":{"type":"string","enum":["vertical","horizontal"]},"ratio":{"type":"number","minimum":0,"maximum":1}},"required":["pane","orientation","ratio"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"string"},"session":{"type":"string"}},"required":["pane","session"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":3,"orientation":"vertical","ratio":0.5},"result":{"pane":"pane:8","session":"session:9"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "workspace.split",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "workspace.closeTab",
        .description = "Closes the named tab and ends every session in its panes. A last tab closes its window, and a last window closes its workspace. An agent needs an attached person to approve this exact operation.",
        .parametersSchema = R"json({"type":"object","properties":{"tab":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["tab"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"tab":{"type":"string"},"sessionsEnded":{"type":"integer","minimum":0}},"required":["tab","sessionsEnded"],"additionalProperties":false})json",
        .example = R"json({"params":{"tab":7},"result":{"tab":"tab:7","sessionsEnded":2}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "workspace.closeTab",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "workspace.closePane",
        .description = "Closes the named pane and ends its session. An empty tab, window or workspace closes with it. An agent needs an attached person to approve this exact operation.",
        .parametersSchema = R"json({"type":"object","properties":{"pane":{"type":"integer","minimum":1,"maximum":4294967295}},"required":["pane"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"string"},"sessionsEnded":{"type":"integer","minimum":0}},"required":["pane","sessionsEnded"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":3},"result":{"pane":"pane:3","sessionsEnded":1}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "workspace.closePane",
        .roles = drivingAttachments,
        .scope = Sync::Scope::Drive,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "consent.respond",
        .description = "Decides one exact pending question of the named kind as its still-attached human recipient; structure permits only deny or once and additionally requires Owner or Driver with Drive scope for the unchanged original operation. A recipient that cannot show the question says unshown, which leaves it to the others asked and refuses it once none is left.",
        .parametersSchema = R"json({"type":"object","properties":{"request":{"type":"integer","minimum":1,"maximum":4294967295},"kind":{"type":"string","enum":["attach","terminate","capability","structure"]},"decision":{"type":"string","enum":["deny","once","session","always","unshown"]}},"required":["request","kind","decision"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","additionalProperties":false})json",
        .example = R"json({"params":{"request":1,"kind":"structure","decision":"deny"},"result":{}})json",
        .principals = { Principal::Person },
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "consent.respond",
        .roles = humanAttachments,
        .scope = Sync::Scope::Observe,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "consent.revoke",
        .description = "Revokes consent on the human caller's exact subscribed session; disconnect requires only a stable attachment, forgetAlways or forgetSession requires only its verified identity key, and disconnectAgents permits neither selector.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":9,"maxLength":18},"action":{"type":"string","enum":["disconnect","forgetAlways","forgetSession","disconnectAgents"]},"attachment":{"type":"string","minLength":32,"maxLength":32},"identity":{"type":"string","minLength":1,"maxLength":65535}},"required":["session","action"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","additionalProperties":false})json",
        .example = R"json({"params":{"session":"session:1","action":"disconnect","attachment":"00000000000000000000000000000001"},"result":{}})json",
        .principals = { Principal::Person },
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "",
        .mcpTool = "",
        .syncMethod = "consent.revoke",
        .roles = humanAttachments,
        .scope = Sync::Scope::Observe,
        .target = CallTarget::Host,
    },
    MethodDescriptor {
        .name = "ubq.split",
        .description = "Splits a pane to the right or downward and starts a shell in the new pane, once the local policy allows it and an attached person approves this exact change; the requesting token must permit driving and creating sessions.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."},"direction":{"type":"string","enum":["right","down"],"default":"right"}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"pane":{"type":"string"},"session":{"type":"string"}},"required":["pane","session"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":"pane:2","direction":"down"},"result":{"pane":"pane:5","session":"session:4"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "split",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::NewSession },
    },
    MethodDescriptor {
        .name = "ubq.kill",
        .description = "Ends a session by hanging up its shell, once the agent policy and the session's person allow it.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"session":{"type":"string"},"terminated":{"type":"boolean"}},"required":["session","terminated"],"additionalProperties":false})json",
        .example = R"json({"params":{"session":"build"},"result":{"session":"session:3","terminated":true}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "kill",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.label",
        .description = "Labels a workspace named with -w, a window with -W, a tab with -t, a pane with -p, or otherwise a session, so later verbs can address it by the label, once the agent policy allows it and the daemon lets this caller change the workspace; an empty label clears it.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."},"tab":{"type":"string","minLength":1,"maxLength":80,"description":"A tab ID, a tab label, or here."},"window":{"type":"string","minLength":1,"maxLength":80,"description":"A window ID, a window label, or here."},"workspace":{"type":"string","minLength":1,"maxLength":80,"description":"A workspace ID, a workspace label, or here."},"label":{"type":"string","maxLength":64}},"required":["label"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"object":{"type":"string"},"label":{"type":"string"}},"required":["object","label"],"additionalProperties":false})json",
        .example = R"json({"params":{"pane":"pane:5","label":"logs"},"result":{"object":"pane:5","label":"logs"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "label",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Drive, Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.tag",
        .description = "Replaces the tags on a workspace named with -w, a window with -W, a tab with -t, a pane with -p, or otherwise a session, once the agent policy allows it and the daemon lets this caller change the workspace; no tags clears them.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."},"tab":{"type":"string","minLength":1,"maxLength":80,"description":"A tab ID, a tab label, or here."},"window":{"type":"string","minLength":1,"maxLength":80,"description":"A window ID, a window label, or here."},"workspace":{"type":"string","minLength":1,"maxLength":80,"description":"A workspace ID, a workspace label, or here."},"tags":{"type":"array","items":{"type":"string","minLength":1,"maxLength":64},"description":"The tags, in the order they are drawn."}},"required":["tags"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"object":{"type":"string"},"tags":{"type":"array","items":{"type":"string"}}},"required":["object","tags"],"additionalProperties":false})json",
        .example = R"json({"params":{"tab":"logs","tags":["production","billing"]},"result":{"object":"tab:7","tags":["production","billing"]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "tag",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Drive, Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.tree",
        .description = "Prints the daemon's workspaces, windows, tabs and panes as an outline, one row a line, each with its name, labels, tags, status and how long since it wrote; words given after the verb keep only the rows that match every one.",
        .parametersSchema = R"json({"type":"object","properties":{"query":{"type":"string","maxLength":256,"description":"Words every row kept must contain."}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"matchCount":{"type":"integer","minimum":0},"rows":{"type":"array","items":{"type":"object"}},"workspaceRevisions":{"type":"array","items":{"type":"object","properties":{"workspace":{"type":"string"},"revision":{"type":"string","minLength":1,"maxLength":20}},"required":["workspace","revision"],"additionalProperties":false}}},"required":["matchCount","rows","workspaceRevisions"]})json",
        .example = R"json({"params":{},"result":{"matchCount":0,"rows":[{"level":"workspace","depth":0,"id":"workspace:1","title":"work"}],"workspaceRevisions":[{"workspace":"workspace:1","revision":"7"}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "tree",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.status",
        .description = "Reports the status records a session's programs set with OSC 7501: idle, working, done, blocked or error, each with its message.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"session":{"type":"string"},"state":{"type":"string","enum":["none","idle","working","done","blocked","error"]},"records":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"app":{"type":"string"},"state":{"type":"string"},"message":{"type":"string"},"title":{"type":"string"},"progress":{"type":"integer"}},"required":["id","state","message"]}}},"required":["session","state","records"],"additionalProperties":false})json",
        .example = R"json({"params":{"session":"build"},"result":{"session":"session:3","state":"blocked","records":[{"id":"job","app":"make","state":"blocked","message":"Continue?","title":""}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "status",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.wait",
        .description = "Waits until a session's command finishes, its foreground returns to the shell, a program reports it is blocked, or its screen shows some text, woken by the daemon's events rather than by asking again, and says so when the events it waited on were dropped.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"A session ID, a session label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"A pane ID, a pane label, or here."},"for":{"type":"string","enum":["exit","idle","blocked","text"]},"text":{"type":"string","minLength":1,"description":"The text to wait for, with for text, which reads the screen and needs the region scope."},"since":{"type":"integer","minimum":0,"maximum":9007199254740991,"description":"An event cursor; events after it count, so nothing between an earlier verb and this wait is missed."},"timeout":{"type":"number","minimum":0,"description":"Seconds to wait; absent waits as long as it takes."}},"required":["for"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"matched":{"type":"boolean"},"for":{"type":"string"},"cursor":{"type":"integer"},"event":{"type":"object"}},"required":["matched","for","cursor"],"additionalProperties":false})json",
        .example = R"json({"params":{"session":"build","for":"exit"},"result":{"matched":true,"for":"exit","cursor":42,"event":{"cursor":42,"kind":"command-finished","session":"session:3","exitStatus":0,"commandLine":"make","directory":"/src"}}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "wait",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.events",
        .description = "Prints the daemon's events as they happen, one per line, resuming after a cursor when given one.",
        .parametersSchema = R"json({"type":"object","properties":{"session":{"type":"string","minLength":1,"maxLength":80,"description":"Only this session's events: an ID, a label, or here."},"pane":{"type":"string","minLength":1,"maxLength":80,"description":"Only the events of this pane's session."},"since":{"type":"integer","minimum":0,"maximum":9007199254740991,"description":"Print every event after this cursor first; zero prints all the daemon holds."},"limit":{"type":"integer","minimum":1,"maximum":9007199254740991,"description":"Stop after this many events."},"timeout":{"type":"number","minimum":0,"description":"Stop after this many seconds."}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"cursor":{"type":"integer"},"kind":{"type":"string","enum":["gap","session-created","session-ended","pane-focused","program-status","command-started","command-finished","bell","title-changed","foreground-changed","consent-requested","consent-granted","consent-denied"]},"session":{"type":"string"},"pane":{"type":"string"}},"required":["cursor","kind"],"description":"Each line printed is one such object."})json",
        .example = R"json({"params":{"since":40},"result":{"cursor":41,"kind":"command-finished","session":"session:3","exitStatus":0,"commandLine":"make","directory":"/src"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "events",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Control },
    },
    MethodDescriptor {
        .name = "ubq.api.list",
        .description = "Lists the operations one surface reaches, by the name it reaches each by: the command line's verbs, the app's control-socket methods, the MCP tools, or every operation, each with a sentence saying what it does and its parameter and result schemas.",
        .parametersSchema = R"json({"type":"object","properties":{"surface":{"type":"string","enum":["command-line","control","mcp","sync","all"],"default":"command-line","description":"Which surface to list: the command line, the app's control socket, the MCP server, a Sync connection's calls, or all of them."}},"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"methods":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"verb":{"type":"string"},"controlMethod":{"type":"string"},"mcpTool":{"type":"string"},"syncMethod":{"type":"string"},"description":{"type":"string"},"params":{"type":"object"},"result":{"type":"object"}},"required":["name","description","params","result"],"additionalProperties":false}}},"required":["methods"],"additionalProperties":false})json",
        .example = R"json({"params":{"surface":"control"},"result":{"methods":[{"name":"send-text","controlMethod":"send-text","description":"Types text verbatim into a pane's program.","params":{"type":"object"},"result":{"type":"object"}}]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "api list",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "ubq.api.describe",
        .description = "Describes one operation by any name it is reached by: its schemas, an example, who may call it, and its name on each surface.",
        .parametersSchema = R"json({"type":"object","properties":{"method":{"type":"string","minLength":1}},"required":["method"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"name":{"type":"string"},"description":{"type":"string"},"params":{"type":"object"},"result":{"type":"object"},"example":{"type":"object"},"principals":{"type":"array","items":{"type":"string"}},"controlMethod":{"type":"string"},"commandLineVerb":{"type":"string"},"mcpTool":{"type":"string"},"syncMethod":{"type":"string"},"roles":{"type":"array","items":{"type":"string","enum":["owner","driver","viewer","agent"]}},"scope":{"type":"string","enum":["observe","drive","control","region","new","file","manage"]},"target":{"type":"string","enum":["pane","host","connection"]},"commandLineScopes":{"type":"array","items":{"type":"string","enum":["observe","drive","control","region","new","file","manage"]}},"origin":{"type":"string","enum":["local"]}},"required":["name","description","params","result","example","principals"],"additionalProperties":false})json",
        .example = R"json({"params":{"method":"wait"},"result":{"name":"ubq.wait","description":"Waits until a command finishes.","params":{"type":"object"},"result":{"type":"object"},"example":{"params":{},"result":{}},"principals":["person","agent"],"commandLineVerb":"wait"}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "api describe",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "ubq.api.call",
        .description = "Calls an operation with its parameters given as JSON, validating them against its schema, and prints its result as JSON.",
        .parametersSchema = R"json({"type":"object","properties":{"method":{"type":"string","minLength":1},"params":{"type":"object","description":"The operation's parameters; absent is an empty object."}},"required":["method"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","description":"The called operation's result."})json",
        .example = R"json({"params":{"method":"ls","params":{}},"result":{"sessions":[]}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "api call",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "ubq.api.validate",
        .description = "Checks a JSON document against an operation's result schema, as a script checks what --json printed, and names where it departs from it.",
        .parametersSchema = R"json({"type":"object","properties":{"method":{"type":"string","minLength":1,"description":"Any name the operation is reached by."},"document":{"type":"object","description":"The result to check; absent is an empty object."}},"required":["method"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"valid":{"type":"boolean"}},"required":["valid"],"additionalProperties":false})json",
        .example = R"json({"params":{"method":"send","document":{"sent":5,"cursor":40}},"result":{"valid":true}})json",
        .principals = commandLineCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "api validate",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "agent.mcp",
        .description = "Serves the MCP tools over standard input and output until the client closes them.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","additionalProperties":false})json",
        .example = R"json({"params":{},"result":{}})json",
        .principals = agentCallers,
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "mcp",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "agent.completions",
        .description = "Prints the command line's completion script for a shell.",
        .parametersSchema = R"json({"type":"object","properties":{"shell":{"type":"string","enum":["bash","zsh"]}},"required":["shell"],"additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"script":{"type":"string"}},"required":["script"],"additionalProperties":false})json",
        .example = R"json({"params":{"shell":"zsh"},"result":{"script":"#compdef ubq\n"}})json",
        .principals = Principals { Principal::Person },
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "completions",
        .mcpTool = "",
    },
    MethodDescriptor {
        .name = "agent.selftest",
        .description = "Attaches to the daemon, runs a command, and reads its output back, reporting whether the round trip works.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"passed":{"type":"boolean"}},"required":["passed"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"passed":true}})json",
        .principals = Principals { Principal::Person },
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "selftest",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::NewSession },
    },
    MethodDescriptor {
        .name = "agent.imagetest",
        .description = "Draws an inline image in a fresh session and checks that its pixels reach the client, reporting whether they did.",
        .parametersSchema = R"json({"type":"object","additionalProperties":false})json",
        .resultSchema = R"json({"type":"object","properties":{"passed":{"type":"boolean"}},"required":["passed"],"additionalProperties":false})json",
        .example = R"json({"params":{},"result":{"passed":true}})json",
        .principals = Principals { Principal::Person },
        .controlMethod = "",
        .controlRoute = ControlRoute::NotServed,
        .commandLineVerb = "imagetest",
        .mcpTool = "",
        .commandLineScopes = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::NewSession },
    },
};

template<typename Field>
const MethodDescriptor* findBy(std::string_view name, Field field, std::span<const MethodDescriptor> table = registry)
{
    if (name.empty())
        return nullptr;
    for (const MethodDescriptor& method : table) {
        if (method.*field == name)
            return &method;
    }
    return nullptr;
}

Value principalNames(Principals principals)
{
    Value names = Value::array();
    if (principals.contains(Principal::Person))
        names.push(Value::string("person"));
    if (principals.contains(Principal::LocalProcess))
        names.push(Value::string("localProcess"));
    if (principals.contains(Principal::Agent))
        names.push(Value::string("agent"));
    return names;
}

void setSurfaceNames(Value& object, const MethodDescriptor& method)
{
    if (!method.controlMethod.empty())
        object.set("controlMethod", Value::string(std::string(method.controlMethod)));
    if (!method.commandLineVerb.empty())
        object.set("commandLineVerb", Value::string(std::string(method.commandLineVerb)));
    if (!method.mcpTool.empty())
        object.set("mcpTool", Value::string(std::string(method.mcpTool)));
    if (!method.syncMethod.empty())
        object.set("syncMethod", Value::string(std::string(method.syncMethod)));
}

} // namespace

std::span<const MethodDescriptor> methodRegistry()
{
    return registry;
}

const MethodDescriptor* findMethod(std::string_view name)
{
    return findBy(name, &MethodDescriptor::name);
}

const MethodDescriptor* findControlMethod(std::string_view method, std::span<const MethodDescriptor> table)
{
    return findBy(method, &MethodDescriptor::controlMethod, table);
}

const MethodDescriptor* findMCPTool(std::string_view tool)
{
    return findBy(tool, &MethodDescriptor::mcpTool);
}

const MethodDescriptor* findCommandLineVerb(std::string_view verb)
{
    return findBy(verb, &MethodDescriptor::commandLineVerb);
}

const MethodDescriptor* findSyncMethod(std::string_view method)
{
    return findBy(method, &MethodDescriptor::syncMethod);
}

const MethodDescriptor* findMethodFromSurface(std::string_view name, Surface surface, std::span<const MethodDescriptor> table)
{
    constexpr std::array<std::string_view MethodDescriptor::*, 4> surfaceFields { &MethodDescriptor::controlMethod, &MethodDescriptor::mcpTool, &MethodDescriptor::commandLineVerb, &MethodDescriptor::syncMethod };
    const auto own = surfaceFields[std::to_underlying(surface)];
    if (const MethodDescriptor* method = findBy(name, own, table))
        return method;
    if (const MethodDescriptor* method = findBy(name, &MethodDescriptor::name, table))
        return method;
    for (const auto field : surfaceFields) {
        if (field == own)
            continue;
        if (const MethodDescriptor* method = findBy(name, field, table))
            return method;
    }
    return nullptr;
}

Value scopeNames(Sync::ScopeSet scopes)
{
    Value names = Value::array();
    for (const Sync::Scope scope : { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::RegionRead, Sync::Scope::NewSession, Sync::Scope::FileRead, Sync::Scope::Manage }) {
        if (scopes.contains(scope))
            names.push(Value::string(Sync::scopeMaskToString(scope)));
    }
    return names;
}

Value parseRegistryJSON(std::string_view text)
{
    bool isParsed = false;
    Value value = Codec::JSON::parse(std::string(text), isParsed);
    return isParsed ? value : Value::object();
}

Value describeMethod(const MethodDescriptor& method)
{
    Value description = Value::object();
    description.set("name", Value::string(std::string(method.name)));
    description.set("description", Value::string(std::string(method.description)));
    description.set("params", parseRegistryJSON(method.parametersSchema));
    description.set("result", parseRegistryJSON(method.resultSchema));
    description.set("example", parseRegistryJSON(method.example));
    description.set("principals", principalNames(method.principals));
    setSurfaceNames(description, method);
    if (!method.commandLineScopes.isEmpty())
        description.set("commandLineScopes", scopeNames(method.commandLineScopes));
    if (!method.syncMethod.empty()) {
        Value roles = Value::array();
        for (const Role role : Sync::everyRole) {
            if (method.roles.contains(role))
                roles.push(Value::string(std::string(Sync::roleName(role))));
        }
        description.set("roles", std::move(roles));
        description.set("scope", Value::string(Sync::scopeMaskToString(method.scope)));
        description.set("target", Value::string(method.target == CallTarget::Host ? "host" : method.target == CallTarget::Connection ? "connection"
                                                                                                                                     : "pane"));
        if (method.origin == CallOrigin::Local)
            description.set("origin", Value::string("local"));
    }
    return description;
}

Value listMethods(Principal principal, std::span<const MethodDescriptor> table)
{
    Value methods = Value::array();
    for (const MethodDescriptor& method : table) {
        if (!method.principals.contains(principal))
            continue;
        Value entry = Value::object();
        entry.set("name", Value::string(std::string(method.name)));
        entry.set("description", Value::string(std::string(method.description)));
        setSurfaceNames(entry, method);
        methods.push(std::move(entry));
    }
    Value result = Value::object();
    result.set("methods", std::move(methods));
    return result;
}

namespace {

/// The first refusal a caller meets that is about who it is, not what it sent.
std::optional<CallRefusal> authorityRefusal(const MethodDescriptor& method, const CallerAuthority& caller)
{
    const std::string name(method.syncMethod);
    if (!method.roles.contains(caller.role))
        return CallRefusal { .code = ErrorCode::Forbidden, .reason = name + " is not open to a caller attached as " + std::string(Sync::roleName(caller.role)) };
    if (method.origin == CallOrigin::Local && caller.origin != CapabilityOrigin::Local)
        return CallRefusal { .code = ErrorCode::Forbidden, .reason = name + " is served only to a client running on the host as its user" };
    if (!caller.scopes.contains(method.scope))
        return CallRefusal { .code = ErrorCode::Forbidden, .reason = name + " needs the " + Sync::scopeMaskToString(method.scope) + " scope, which the caller's credential does not carry" };
    return std::nullopt;
}

} // namespace

std::optional<CallRefusal> admitCall(const MethodDescriptor* method, const Value& parameters, const CallerAuthority& caller)
{
    if (!method || method->syncMethod.empty())
        return CallRefusal { .code = ErrorCode::MethodNotFound, .reason = "the host serves no such method" };
    if (std::optional<CallRefusal> refusal = authorityRefusal(*method, caller))
        return refusal;
    if (const std::optional<SchemaViolation> violation = validate(parameters, parseRegistryJSON(method->parametersSchema)))
        return CallRefusal { .code = ErrorCode::InvalidParameters, .reason = std::string(method->syncMethod) + ": " + (violation->path.empty() ? std::string("the parameters") : violation->path) + " " + violation->message };
    return std::nullopt;
}

Value listSyncMethods(const CallerAuthority& caller, std::span<const MethodDescriptor> table)
{
    Value methods = Value::array();
    for (const MethodDescriptor& method : table) {
        if (method.syncMethod.empty() || authorityRefusal(method, caller))
            continue;
        Value entry = Value::object();
        entry.set("name", Value::string(std::string(method.syncMethod)));
        entry.set("description", Value::string(std::string(method.description)));
        entry.set("target", Value::string(method.target == CallTarget::Host ? "host" : method.target == CallTarget::Connection ? "connection"
                                                                                                                               : "pane"));
        entry.set("scope", Value::string(Sync::scopeMaskToString(method.scope)));
        methods.push(std::move(entry));
    }
    Value result = Value::object();
    result.set("methods", std::move(methods));
    return result;
}

} // namespace Ubiquitty::Protocol::Control
