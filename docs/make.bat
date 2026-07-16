@ECHO OFF

pushd %~dp0

REM Command file for Sphinx documentation

set _OVSTAGE_SPHINXBUILD_DEFAULT=
if "%SPHINXBUILD%" == "" (
	set SPHINXBUILD=sphinx-build
	set _OVSTAGE_SPHINXBUILD_DEFAULT=1
)
set SOURCEDIR=.
set BUILDDIR=_build

REM Provision Sphinx from python\pyproject.toml [docs] extra via uv when we set
REM the default SPHINXBUILD (a user-provided %SPHINXBUILD% is respected as-is).
if not "%_OVSTAGE_SPHINXBUILD_DEFAULT%" == "1" goto check_sphinx
where uv >NUL 2>NUL
if errorlevel 1 goto check_sphinx
echo Using uv-managed environment from python\pyproject.toml [docs] extra...
set SPHINXBUILD=uv run --project ..\python --extra docs sphinx-build

:check_sphinx
%SPHINXBUILD% >NUL 2>NUL
if errorlevel 9009 (
	echo.
	echo.The 'sphinx-build' command was not found. Make sure you have Sphinx
	echo.installed, then set the SPHINXBUILD environment variable to point
	echo.to the full path of the 'sphinx-build' executable. Alternatively you
	echo.may add the Sphinx directory to PATH.
	echo.
	echo.If you don't have Sphinx installed, grab it from
	echo.https://www.sphinx-doc.org/
	echo.
	echo.Tip: install uv from https://docs.astral.sh/uv/ and re-run; this script
	echo.will automatically use uv to provision Sphinx from python\pyproject.toml.
	exit /b 1
)

if "%1" == "" goto help

%SPHINXBUILD% -M %1 %SOURCEDIR% %BUILDDIR% %SPHINXOPTS% %O%
goto end

:help
%SPHINXBUILD% -M help %SOURCEDIR% %BUILDDIR% %SPHINXOPTS% %O%

:end
popd
