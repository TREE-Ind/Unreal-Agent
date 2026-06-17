## Unreal Agent – Unreal Engine Editor AI Agent Plugin

Unreal Agent is an **AI-powered editor copilot** for Unreal Engine.  
It runs *inside* the editor as a dockable tab, talks to OpenAI’s GPT models, and can **inspect and modify your project** using Python, scene queries, screenshots, and external tools.

- **Engine**: Unreal Engine 5.6(current)  
- **Type**: Editor + runtime plugin (`UnrealGPTEditor`, `UnrealGPT`)  
- **Category**: Developer Tools

---

### Key Features

- **In‑editor chat assistant**
  - Dockable `UnrealGPT` tab under `Window → UnrealGPT`.
  - `Ctrl+Enter` to send messages.

- **Scene understanding & context capture**
  - `Capture Context` button:
    - Captures a **viewport screenshot**.
    - Streams a **JSON scene summary** of actors/components in the current level.
  - `scene_query` tool to search actors by class/name/label/component types.
  - `GetSelectedActorsSummary` for focused summaries of current selection (used internally).

- **Action‑based agent with Python tooling**
  - `python_execute` tool runs **Python editor scripts** directly in UE.
  - Agent is instructed to **change the project/level for you**, not just give instructions.
  - Built‑in reflection helper (`reflection_query`) to inspect `UClass` properties and functions.
  - Standard JSON `result` contract for Python code:
    - `status`, `message`, and rich `details` (e.g. created actor/asset info).

- **Documentation & web tools**
  - `file_search` tool against a **UE 5.6 Python API vector store** (OpenAI `file_search`).
  - `web_search` tool to query web docs (OpenAI Responses tools).
  - Local UE 5.6 Python docs shipped in `ue5_python_api_docs*` for reference and indexing.

- **Viewport screenshots & visual feedback**
  - `viewport_screenshot` tool:
    - Captures the active editor viewport as PNG.
    - Shows the screenshot inline in the chat history.

- **Voice input (Whisper)**
  - Press the microphone button to record from your default input device.
  - Audio is sent to **OpenAI Whisper (`/v1/audio/transcriptions`, `whisper-1`)**.
  - Transcription text is inserted into the chat input for review before sending.

- **Replicate content generation (optional)**
  - Optional `replicate_generate` tool for images, 3D, audio, music, speech, and video.
  - Direct HTTP integration with Replicate’s Predictions API.
  - Python helpers (`Content/Python/unrealgpt_mcp_import.py`) to import generated files as:
    - `Texture2D`
    - `StaticMesh`
    - `SoundWave`

- **Safety & guardrails**
  - Tool‑loop protection with a maximum tool‑call iteration count.
  - Tool result size limits to avoid blowing up context.
  - Execution timeout setting for risky/long‑running Python code.

---

### Requirements

- **Unreal Engine 5.6.0** (plugin `EngineVersion` is 5.6.0).
- **Desktop OS**: Developed and tested on Windows; other platforms may work but are not guaranteed.
- **Internet access** to reach:
  - OpenAI API endpoint configured in settings (default: `https://api.openai.com/v1/responses`).
  - Optional Replicate API endpoint (default: `https://api.replicate.com/v1/predictions`).
- **OpenAI‑compatible API key** with access to:
  - Chosen GPT model (default `gpt-5.1`).
  - `responses` endpoint.
  - `audio/transcriptions` for Whisper.
  - `web_search` / `file_search` tools if you plan to use them.
- **Python editor scripting**:
  - Enable **“Python Editor Script Plugin”** (and any dependent Python plugins) in your project/engine.
  - A working Python environment that Unreal’s Python plugin can use.

Optional:

- **Replicate account + API token** if you want to use `replicate_generate`.

---

### Installation

1. **Copy the plugin into your project**
   - Place this folder as:
     - `YourProject/Plugins/UnrealGPT`  (recommended), or
     - `<UE_5.6_Install>/Engine/Plugins/Developer/UnrealGPT`  (engine‑wide).

2. **Regenerate project files (C++ projects only)**
   - Right‑click your `.uproject` → **Generate Visual Studio project files** (or your IDE of choice).

3. **Open the project in UE5.6**
   - Launch the editor for your project.

4. **Enable the plugin**
   - Go to **Edit → Plugins → Developer Tools** (or search for `UnrealGPT`).
   - Enable **UnrealGPT**.
   - Restart the editor if prompted.

5. **Enable Python editor scripting**
   - In **Edit → Plugins**, enable **Python Editor Script Plugin**.
   - Restart the editor once more if required.

---

### Configuration

All plugin settings live under:

> **Edit → Project Settings → Plugins → UnrealGPT**

Key settings (`UUnrealGPTSettings`):

- **API**
  - **Base URL Override**
    - Optional; overrides only the base URL portion of the API endpoint (e.g. point at a proxy or self‑hosted gateway).
  - **API Endpoint**
    - Default: `https://api.openai.com/v1/responses`.
  - **API Key**
    - Your OpenAI (or compatible) API key.
    - Used for both chat/responses and Whisper audio transcription.

- **Model**
  - **Default Model**
    - Default: `gpt-5.1`.
    - Any Responses‑capable model is supported; models with native reasoning (e.g. `gpt-5.*`, `o1`, `o3`) receive additional reasoning configuration automatically.

- **Tools**
  - **Enable Python Execution**
    - Toggle the `python_execute` tool on/off.
    - Requires Python editor scripting support in the engine.
  - **Enable Viewport Screenshot**
    - Controls access to `viewport_screenshot` (and screenshot capture used by `Capture Context`).
  - **Enable Scene Summary**
    - Controls `GetSceneSummary` / `scene_query`‑based tools.

- **Replicate (optional)**
  - **Enable Replicate Tool**
    - Enables `replicate_generate` in the tool list.
  - **Replicate API Token**
    - Token from your Replicate account (the “Token” value, not your password).
  - **Replicate API URL**
    - Default: `https://api.replicate.com/v1/predictions`.
  - **Image / 3D / SFX / Music / Speech / Video Models**
    - Default model identifiers per content type.
    - Used when the agent doesn’t specify a `version` directly.

- **Safety**
  - **Execution Timeout (seconds)**
    - Upper bound for Python tool execution.
  - **Max Context Tokens**
    - Approximate cap on total tokens sent per request (used when building request payloads).

- **Context**
  - **Scene Summary Page Size**
    - Pagination size for world summaries (`GetSceneSummary`).

---

### Using UnrealGPT in the Editor

1. **Open the UnrealGPT tab**
   - In the editor main menu, open **Window → UnrealGPT**.
   - This opens a dockable tab containing the chat UI.

2. **Send your first message**
   - Type into the input box at the bottom:
     - Example: *“Add three point lights above the player start and align them neatly.”*
   - Press **Ctrl+Enter** or click **Send**.

3. **Capture context from the current level**
   - Click **“Capture Context”** in the top toolbar:
     - Sends a **scene summary** (actors, transforms, components) to the agent.
     - Captures a **viewport screenshot** (if enabled).
     - The agent can then reason about *what it sees* before taking action.

4. **Use voice input (optional)**
   - Click the **microphone icon**:
     - Recording starts (icon turns red while recording).
   - Click again to stop; audio is sent to Whisper and the transcribed text is inserted into the input box.
   - Review or edit the transcription, then send as usual.

5. **Attach images**
   - Click the **paperclip icon** to select a local image (`.png`, `.jpg`, `.jpeg`).
   - Attached images are base64‑encoded and included with your next message.
   - A small label indicates how many images are attached.

6. **Manage the conversation**
   - **Clear History**: Resets the agent’s conversation state and clears the chat UI.
   - **Reasoning strip**:
     - A small strip above the input shows brief reasoning or “Thinking…” while the model is working.
   - **Tool activity**
     - Tool calls (Python execution, scene queries, screenshots, Replicate, etc.) appear as **distinct, color‑coded cards** in the history.
     - Tool results are summarized in a human‑readable format (e.g. numbered lists for `scene_query`).

---

### Tools Overview (What the Agent Can Do)

The plugin configures a set of tools that the model can call autonomously:

- **`python_execute`**
  - Runs arbitrary Python inside the Unreal Editor process.
  - Intended for:
    - Creating and modifying actors.
    - Working with Blueprints and assets.
    - Batch operations in the Content Browser.
  - Python code should read/write a shared `result` dictionary (see comments in `UnrealGPTAgentClient.cpp`).
  - Supports using helper modules like `unrealgpt_mcp_import` to import generated files.

- **`scene_query`**
  - Searches the world using simple filters:
    - `class_contains`, `label_contains`, `name_contains`, `component_class_contains`, `max_results`.
  - Returns a compact JSON array of matches with:
    - `name`, `label`, `class`, and `location` (x, y, z).

- **`viewport_screenshot`**
  - Captures the active viewport and returns a PNG screenshot (base64).
  - The UI decodes and displays the screenshot inline.

- **`reflection_query`**
  - Inspects a `UClass` and returns a JSON “schema” for its:
    - Properties (names, C++/UE types, flags).
    - Functions (parameters, return types, flags).
  - Helps the model write correct Python against Unreal types.

- **`file_search` / `web_search`** (Responses API only)
  - `file_search` is configured to use a UE 5.6 Python API vector store.
  - `web_search` allows broader web queries (e.g. docs and examples).
  - Both are native OpenAI tools invoked via the Responses API.

- **`replicate_generate`** (optional)
  - Available when **Replicate Tool** is enabled and a **Replicate API Token** is set.
  - Generates content (images, video, audio, or 3D files) via Replicate.
  - Returns JSON with:
    - `status`, `message`.
    - `details.files[*].local_path`, `mime_type`, `inferred_usage`.
  - Use with `python_execute` and `unrealgpt_mcp_import` to turn files into UE assets.

> **Note**  
> A “Computer Use” tool is stubbed out in the codebase but currently disabled for safety.

---

### Python Helpers for MCP / Replicate Imports

The plugin ships with a Python helper module:

- `Content/Python/unrealgpt_mcp_import.py`

It provides functions the agent (or you) can call from `python_execute`:

- `import_mcp_texture(mcp_result_json, target_folder, asset_name_hint=None)`
  - Imports an image file as a `Texture2D`.
- `import_mcp_static_mesh(mcp_result_json, target_folder, asset_name_hint=None)`
  - Imports a 3D model file as a `StaticMesh`.
- `import_mcp_audio(mcp_result_json, target_folder, asset_name_hint=None)`
  - Imports an audio file as a `SoundWave`.

Each helper:

- Parses a result JSON (e.g. from `replicate_generate` or MCP), finds the relevant file,
- Runs an `AssetImportTask`, and
- Returns a small JSON dict with:
  - `status`, `message`, and `details.asset_path` / `details.local_path`.

---

### Tips & Best Practices

- **Start with “Capture Context”** for scene‑related requests so the agent has up‑to‑date information.
- **Prefer Python automation**:
  - Ask for high‑level goals (e.g. *“Set up a lighting rig in this level”*) and let the agent script it.
- **Let the agent verify its work**:
  - The agent is instructed to use `scene_query` and `viewport_screenshot` after `python_execute` to confirm success.
- **Use `file_search` for API questions**:
  - Ask things like *“How do I spawn actors with EditorActorSubsystem in UE 5.6 Python?”* and let the agent call `file_search` first.
- **Keep an eye on Python output**:
  - If something fails, the `python_execute` result JSON (and any tracebacks) are surfaced in the tool result cards.

---

### Troubleshooting

- **UnrealGPT tab does not appear**
  - Ensure the plugin is enabled under **Edit → Plugins → UnrealGPT**.
  - Check the Output Log for any module load errors for `UnrealGPT` or `UnrealGPTEditor`.

- **“API Key not set in settings” in log**
  - Open **Project Settings → Plugins → UnrealGPT**.
  - Set a valid **API Key**, click **Save**, then try again.

- **Voice input fails or records silence**
  - Confirm your system has a default input device and microphones are allowed.
  - Check the Output Log for messages from `UnrealGPTVoiceInput`.

- **Python execution errors**
  - Make sure **Python Editor Script Plugin** is enabled.
  - Look for Python tracebacks in tool results or the Output Log.
  - If needed, temporarily log more details from your Python scripts.

- **Replicate tool reports configuration errors**
  - Verify:
    - **Enable Replicate Tool** is checked.
    - **Replicate API Token** is populated.
    - Appropriate **Image/3D/Audio/etc. model IDs** are set.

- **file_search returns errors or no results**
  - `file_search` is tied to a specific vector store ID in `UnrealGPTAgentClient.cpp`.
  - If your API key does not have access to that store, you can:
    - Create your own UE 5.6 Python docs vector store, and
    - Update the vector store ID in the source code, then rebuild the plugin.

---

### Development Notes

- Modules:
  - `UnrealGPT` (runtime, minimal) – standard module skeleton.
  - `UnrealGPTEditor` (editor) – UI, agent client, tools, voice input, and settings.
- The chat UI is implemented in `SUnrealGPTWidget` with a modern AAA‑style layout:
  - Toolbar (`Capture Context`, `Clear History`, `Settings`).
  - Scrollable chat history with message bubbles and tool cards.
  - Input row with multiline text, voice button, image attach, and send button.
- Fonts:
  - Uses the bundled **Geist** and **Geist Mono** fonts where available.
  - Falls back to standard editor fonts if fonts cannot be loaded from plugin content.

---

### Support & Credits

- **Author**: TREE Industries  
- **Plugin Name**: `UnrealGPT`  
- **Description**: “AI-powered agent assistant for Unreal Engine 5.6 with code execution and computer use capabilities”


