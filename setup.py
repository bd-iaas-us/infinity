import subprocess
from setuptools import setup, find_packages, Extension
from setuptools.command.build_ext import build_ext


def get_version():
    try:
        latest_tag = (
            subprocess.check_output(["git", "describe", "--tags", "--abbrev=0"])
            .strip()
            .decode()
        )

        commit_count = (
            subprocess.check_output(
                ["git", "rev-list", f"{latest_tag}..HEAD", "--count"]
            )
            .strip()
            .decode()
        )

        return f"{latest_tag}.{commit_count}"
    except subprocess.CalledProcessError:
        raise Exception("Please make sure you have git installed and have a tag number")


# invoke the make command to build the shared library
class CustomBuildExt(build_ext):
    def run(self):
        import glob
        import shutil
        import os

        subprocess.check_call(["meson", "setup", "build", "--wipe"], cwd="src")
        subprocess.check_call(["ninja"], cwd="src/build")

        so_files = glob.glob("src/build/_infinistore*.so")
        if self.inplace:
            for so_file in so_files:
                dest = os.path.join("infinistore", os.path.basename(so_file))
                print(f"Copying {so_file} to {dest}")
                shutil.copy(so_file, dest)
        else:
            build_dir = os.path.join(self.build_lib, "infinistore")
            for so_file in so_files:
                build_dest = os.path.join(build_dir, os.path.basename(so_file))
                print(f"Copying {so_file} to build directory: {build_dest}")
                shutil.copy(so_file, build_dest)


cpp_extension = Extension(name="infinistore._infinistore", sources=[])
ext_modules = [cpp_extension]

setup(
    name="infinistore",
    version=get_version(),
    packages=find_packages(),
    cmdclass={"build_ext": CustomBuildExt},
    package_data={
        "infinistore": ["_infinistore*.so"],
    },
    install_requires=[
        "uvloop",
        "fastapi",
        "uvicorn",
        "numpy",
    ],
    description="A kvcache memory pool",
    long_description=open("README.md").read(),
    long_description_content_type="text/markdown",
    url="https://github.com/bd-iaas-us/infiniStore",
    entry_points={
        "console_scripts": [
            "infinistore=infinistore.server:main",
        ],
    },
    ext_modules=ext_modules,
    zip_safe=False,
)
