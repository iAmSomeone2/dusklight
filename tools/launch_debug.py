#!/usr/bin/env python3.12
"""
This is helper script used to make it easier to launch Dusklight in debug mode using a CMake preset.

It reads the CMakePresets.json file to find the appropriate 'default-debug' preset for the current platform and executes
the build and binary with debug arguments.
"""

import json
import os
import sys
import subprocess

_CMAKE_CONFIG_PRESETS = json.load(open(os.path.join(os.path.dirname(__file__), "../CMakePresets.json")))["configurePresets"]

_CMAKE_SOURCE_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))

_DEBUG_ARGS = ["-l", "1", "--dvd", os.path.join(_CMAKE_SOURCE_DIR, "orig", "GZ2E01", "GZ2E01.iso"), "--console"]


class CMakePreset:
    """
    Structured representation of a CMake preset, including its name, platform, display name, binary directory, and cache variables.
    It also supports inheritance from other presets and provides methods to retrieve the build directory and binary path.
    """

    name: str
    platform: str | None = None
    inherits_from: set["CMakePreset"] = set()
    """References to the presets that this preset inherits from"""
    display_name: str
    binary_dir: str | None = None
    cache_variables: dict[str, str] | None = None

    def __init__(self, json_preset, other_presets: dict[str, "CMakePreset"]):
        """
        Initializes a CMakePreset instance from a JSON preset dictionary and a dictionary of other presets for inheritance.

        Args:
            json_preset (dict): The JSON representation of the preset.
            other_presets (dict[str, CMakePreset]): A dictionary of other CMakePreset instances, keyed by their names, to support inheritance.
        """

        self.name = json_preset.get("name")
        self.display_name = json_preset.get("displayName")
        self.binary_dir = json_preset.get("binaryDir")
        self.cache_variables = json_preset.get("cacheVariables")

        self.inherits_from = {preset for preset in other_presets.values() if preset.name in json_preset.get("inherits", [])}

        target_os = self.name.split("-")[0]
        if target_os not in ["linux", "windows", "macos"]:
            self.platform = None
        elif target_os == "macos":
            # Python considers the platform to be "darwin" on macOS, but our presets use "macos" as the target OS.
            self.platform = "darwin"
        else:
            self.platform = target_os

    def __repr__(self):
        return f"CMakePreset(name={self.name}, platform={self.platform}, display_name={self.display_name}, cache_variables={self.cache_variables})"

    def get_build_dir(self, is_parent: bool = False) -> str | None:
        binary_dir = self.binary_dir
        if not binary_dir:
            for parent in self.inherits_from:
                parent_binary_dir = parent.get_build_dir(is_parent=True)
                if parent_binary_dir:
                    binary_dir = parent_binary_dir

        if not binary_dir:
            return None

        if is_parent:
            # return the binary_dir as is for parent presets without replacing variables
            return binary_dir

        # Replace ${sourceDir} with the actual source directory
        binary_dir = binary_dir.replace(r"${sourceDir}", _CMAKE_SOURCE_DIR)
        # Replace ${presetName} with the actual preset name
        binary_dir = binary_dir.replace(r"${presetName}", self.name)
        return binary_dir

    def get_binary_path(self) -> str | None:
        binary_dir = self.get_build_dir()
        if not binary_dir:
            return None

        if self.platform == "darwin":
            return os.path.join(binary_dir, "Dusklight.app", "Contents", "MacOS", "Dusklight")
        else:
            # For Linux and Windows, the binary is located directly in the binary directory
            return os.path.join(binary_dir, "Dusklight")

    def exec_build(self):
        cmake_cmd = ["cmake", "--build", "--preset", self.name]
        print(f"Executing build command: {' '.join(cmake_cmd)}")
        subprocess.run(cmake_cmd, check=True)

    def exec_bin(self):
        binary_path = self.get_binary_path()
        if not binary_path:
            print("Binary path not found.")
            return

        print(f"Executing binary: {binary_path}")
        subprocess.run([binary_path] + _DEBUG_ARGS, check=True)


class CMakePresetManager:
    presets: dict[str, CMakePreset] = {}

    def __init__(self):
        for preset in _CMAKE_CONFIG_PRESETS:
            preset_name = preset.get("name")
            self.presets[preset_name] = CMakePreset(preset, self.presets)

    def get_all_for_platform(self, platform: str) -> dict[str, CMakePreset]:
        return {name: preset for name, preset in self.presets.items() if preset.platform == platform}

    def get_all_for_current_platform(self) -> dict[str, CMakePreset]:
        current_platform = sys.platform
        return self.get_all_for_platform(current_platform)

    def get_preset(self, name: str) -> CMakePreset | None:
        return self.presets.get(name)

    def get_for_current_platform_by_suffix(self, key: str) -> CMakePreset | None:
        platform_presets = self.get_all_for_current_platform()

        for preset in platform_presets.values():
            if preset.name.split('-', 1)[-1] == key:
                return preset
        return None


_PRESET_MANAGER = CMakePresetManager()

if __name__ == "__main__":
    debug_preset = _PRESET_MANAGER.get_for_current_platform_by_suffix("default-debug")
    if debug_preset:
        debug_preset.exec_build()
        debug_preset.exec_bin()
