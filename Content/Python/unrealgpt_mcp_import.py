"""
UnrealGPT MCP Import Helpers

These functions help import MCP-generated content (images, 3D models, audio) into Unreal Engine assets.
They can be called from python_execute tool calls.

Example usage:
    import unrealgpt_mcp_import
    mcp_result_json = '{"status":"ok","details":{"files":[{"local_path":"C:/path/to/file.png","inferred_usage":"image"}]}}'
    asset_path = unrealgpt_mcp_import.import_mcp_texture(mcp_result_json, "/Game/MyTextures", "MyTexture")
"""

import unreal
import json
import os


def import_mcp_texture(mcp_result_json, target_folder, asset_name_hint=None):
    """
    Import an image file from MCP result as a Texture2D asset.
    
    Args:
        mcp_result_json: JSON string from mcp_call tool result
        target_folder: Target asset folder (e.g., "/Game/Textures")
        asset_name_hint: Optional name hint for the asset (will use filename if not provided)
    
    Returns:
        dict with 'status', 'message', 'details' containing asset_path
    """
    result = {
        "status": "ok",
        "message": "",
        "details": {}
    }
    
    try:
        mcp_data = json.loads(mcp_result_json)
        if mcp_data.get("status") != "ok":
            result["status"] = "error"
            result["message"] = f"MCP call failed: {mcp_data.get('message', 'Unknown error')}"
            return result
        
        files = mcp_data.get("details", {}).get("files", [])
        if not files:
            result["status"] = "error"
            result["message"] = "No files found in MCP result"
            return result
        
        # Find first image file
        image_file = None
        for file_info in files:
            usage = file_info.get("inferred_usage", "").lower()
            mime_type = file_info.get("mime_type", "").lower()
            if usage == "image" or mime_type.startswith("image/"):
                image_file = file_info
                break
        
        if not image_file:
            result["status"] = "error"
            result["message"] = "No image file found in MCP result"
            return result
        
        local_path = image_file.get("local_path", "")
        if not local_path or not os.path.exists(local_path):
            result["status"] = "error"
            result["message"] = f"Image file not found: {local_path}"
            return result
        
        # Determine asset name
        if not asset_name_hint:
            asset_name_hint = os.path.splitext(os.path.basename(local_path))[0]
        
        # Import using AssetTools
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        texture_factory = unreal.TextureFactory()
        
        import_task = unreal.AssetImportTask()
        import_task.filename = local_path
        import_task.destination_path = target_folder
        import_task.destination_name = asset_name_hint
        import_task.replace_existing = True
        import_task.automated = True
        import_task.save = True
        
        asset_tools.import_asset_tasks([import_task])
        
        if import_task.imported_object_paths:
            asset_path = import_task.imported_object_paths[0]
            result["message"] = f"Successfully imported texture: {asset_path}"
            result["details"]["asset_path"] = asset_path
            result["details"]["local_path"] = local_path
        else:
            result["status"] = "error"
            result["message"] = "Import task completed but no asset was created"
    
    except Exception as e:
        result["status"] = "error"
        result["message"] = str(e)
        import traceback
        result["details"]["traceback"] = traceback.format_exc()
    
    return result


def import_mcp_static_mesh(mcp_result_json, target_folder, asset_name_hint=None):
    """
    Import a 3D model file from MCP result as a StaticMesh asset.
    
    Args:
        mcp_result_json: JSON string from mcp_call tool result
        target_folder: Target asset folder (e.g., "/Game/Meshes")
        asset_name_hint: Optional name hint for the asset (will use filename if not provided)
    
    Returns:
        dict with 'status', 'message', 'details' containing asset_path
    """
    result = {
        "status": "ok",
        "message": "",
        "details": {}
    }
    
    try:
        mcp_data = json.loads(mcp_result_json)
        if mcp_data.get("status") != "ok":
            result["status"] = "error"
            result["message"] = f"MCP call failed: {mcp_data.get('message', 'Unknown error')}"
            return result
        
        files = mcp_data.get("details", {}).get("files", [])
        if not files:
            result["status"] = "error"
            result["message"] = "No files found in MCP result"
            return result
        
        # Find first 3D model file
        mesh_file = None
        for file_info in files:
            usage = file_info.get("inferred_usage", "").lower()
            mime_type = file_info.get("mime_type", "").lower()
            if usage == "3d_model" or "model" in mime_type or "mesh" in mime_type.lower():
                mesh_file = file_info
                break
        
        if not mesh_file:
            result["status"] = "error"
            result["message"] = "No 3D model file found in MCP result"
            return result
        
        local_path = mesh_file.get("local_path", "")
        if not local_path or not os.path.exists(local_path):
            result["status"] = "error"
            result["message"] = f"Mesh file not found: {local_path}"
            return result
        
        # Determine asset name
        if not asset_name_hint:
            asset_name_hint = os.path.splitext(os.path.basename(local_path))[0]
        
        # Import using AssetTools
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        static_mesh_factory = unreal.StaticMeshFactory()
        
        import_task = unreal.AssetImportTask()
        import_task.filename = local_path
        import_task.destination_path = target_folder
        import_task.destination_name = asset_name_hint
        import_task.replace_existing = True
        import_task.automated = True
        import_task.save = True
        
        asset_tools.import_asset_tasks([import_task])
        
        if import_task.imported_object_paths:
            asset_path = import_task.imported_object_paths[0]
            result["message"] = f"Successfully imported static mesh: {asset_path}"
            result["details"]["asset_path"] = asset_path
            result["details"]["local_path"] = local_path
        else:
            result["status"] = "error"
            result["message"] = "Import task completed but no asset was created"
    
    except Exception as e:
        result["status"] = "error"
        result["message"] = str(e)
        import traceback
        result["details"]["traceback"] = traceback.format_exc()
    
    return result


def import_mcp_audio(mcp_result_json, target_folder, asset_name_hint=None):
    """
    Import an audio file from MCP result as a SoundWave asset.
    
    Args:
        mcp_result_json: JSON string from mcp_call tool result
        target_folder: Target asset folder (e.g., "/Game/Audio")
        asset_name_hint: Optional name hint for the asset (will use filename if not provided)
    
    Returns:
        dict with 'status', 'message', 'details' containing asset_path
    """
    result = {
        "status": "ok",
        "message": "",
        "details": {}
    }
    
    try:
        mcp_data = json.loads(mcp_result_json)
        if mcp_data.get("status") != "ok":
            result["status"] = "error"
            result["message"] = f"MCP call failed: {mcp_data.get('message', 'Unknown error')}"
            return result
        
        files = mcp_data.get("details", {}).get("files", [])
        if not files:
            result["status"] = "error"
            result["message"] = "No files found in MCP result"
            return result
        
        # Find first audio file
        audio_file = None
        for file_info in files:
            usage = file_info.get("inferred_usage", "").lower()
            mime_type = file_info.get("mime_type", "").lower()
            if usage == "audio" or mime_type.startswith("audio/"):
                audio_file = file_info
                break
        
        if not audio_file:
            result["status"] = "error"
            result["message"] = "No audio file found in MCP result"
            return result
        
        local_path = audio_file.get("local_path", "")
        if not local_path or not os.path.exists(local_path):
            result["status"] = "error"
            result["message"] = f"Audio file not found: {local_path}"
            return result
        
        # Determine asset name
        if not asset_name_hint:
            asset_name_hint = os.path.splitext(os.path.basename(local_path))[0]
        
        # Import using AssetTools
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        sound_factory = unreal.SoundFactory()
        
        import_task = unreal.AssetImportTask()
        import_task.filename = local_path
        import_task.destination_path = target_folder
        import_task.destination_name = asset_name_hint
        import_task.replace_existing = True
        import_task.automated = True
        import_task.save = True
        
        asset_tools.import_asset_tasks([import_task])
        
        if import_task.imported_object_paths:
            asset_path = import_task.imported_object_paths[0]
            result["message"] = f"Successfully imported audio: {asset_path}"
            result["details"]["asset_path"] = asset_path
            result["details"]["local_path"] = local_path
        else:
            result["status"] = "error"
            result["message"] = "Import task completed but no asset was created"
    
    except Exception as e:
        result["status"] = "error"
        result["message"] = str(e)
        import traceback
        result["details"]["traceback"] = traceback.format_exc()
    
    return result


