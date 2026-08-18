#!/usr/bin/env python3

import subprocess
import os

_IMAGE_NAME = "localhost/dusklight-builder:latest"


def derive_root_project_dir():
    """
    Derive the root project directory from this script's filesystem path.
    :return:
    """
    script_dir = os.path.dirname(os.path.realpath(__file__))
    script_dir_parts = script_dir.split(os.sep)
    while script_dir_parts and script_dir_parts[-1] != "dusklight":
        script_dir_parts.pop()
    if not script_dir_parts:
        raise RuntimeError("Could not find root project directory (dusklight) from script path")
    return os.sep.join(script_dir_parts)


def build_dusklight():
    """
    Build the dusklight project using a podman container.
    """

    check_container_exists_cmd = ["podman", "ps", "-a", "--filter", "name=dusklight-builder", "--format", "{{.Names}}"]
    existing_containers = subprocess.run(check_container_exists_cmd, capture_output=True, text=True)
    if "dusklight-builder" in existing_containers.stdout:
        print("Removing existing 'dusklight-builder' container...")
        subprocess.run(["podman", "rm", "-f", "dusklight-builder"], check=True)

    build_cmd = [
        "podman", "run", "-it",
        "-v", "dusklight-sccache:/workspace/cache",
        "-v", "dusklight-build:/workspace/build",
        "--mount", f"type=bind,source={derive_root_project_dir()},target=/workspace/src,ro",
        "--name", "dusklight-builder",
        _IMAGE_NAME,
    ]

    subprocess.run(build_cmd, check=True)


def copy_build_artifacts():
    """
    Copy build artifacts from the podman container volume to the host filesystem.
    """
    build_dir = os.path.join(derive_root_project_dir(), "build", "raspberry-pi")
    if os.path.exists(build_dir):
        print(f"Removing existing build directory: {build_dir}")
        subprocess.run(["rm", "-rf", build_dir], check=True)
    os.makedirs(build_dir, exist_ok=True)

    copy_cmd = [
        "podman", "cp",
        "dusklight-builder:/workspace/build/install",  # Source path inside the container
        build_dir  # Destination path on the host
    ]

    subprocess.run(copy_cmd, check=True)


def cleanup_container():
    """
    Clean up the podman container after the build is complete.
    """
    cleanup_cmd = [
        "podman", "rm", "-f", "dusklight-builder"
    ]

    subprocess.run(cleanup_cmd, check=True)


if __name__ == "__main__":
    print("Starting build process for Dusklight on Raspberry Pi...")
    build_dusklight()
    print("Build process completed. Copying build artifacts...")
    copy_build_artifacts()
    print("Build artifacts copied. Cleaning up container...")
    cleanup_container()
