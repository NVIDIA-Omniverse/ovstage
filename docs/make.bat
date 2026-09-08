@ECHO OFF

setlocal

pushd "%~dp0"
if errorlevel 1 exit /b 1

REM Command file for Sphinx documentation

set _OVSTAGE_EXIT_CODE=0
set _OVSTAGE_SPHINXBUILD_DEFAULT=
if "%SPHINXBUILD%" == "" (
	set SPHINXBUILD=sphinx-build
	set _OVSTAGE_SPHINXBUILD_DEFAULT=1
)
set SOURCEDIR=.
set BUILDDIR=_build
if "%DOXYGEN%" == "" (
	set "DOXYGEN=doxygen"
	if exist "%~dp0_tools\doxygen-1.17.0\doxygen.exe" set "DOXYGEN=%~dp0_tools\doxygen-1.17.0\doxygen.exe"
)

if /I "%~1" == "doxygen" goto run_doxygen
if /I "%~1" == "clean" goto clean
if /I "%~1" == "html" goto html
goto setup_sphinx

REM Keep the HTML target in sync with the Makefile: Breathe needs Doxygen XML.
:html
set _OVSTAGE_BUILD_HTML=1
goto run_doxygen

:setup_sphinx
REM Provision Sphinx from python\pyproject.toml [docs] extra via uv when we set
REM the default SPHINXBUILD (a user-provided %SPHINXBUILD% is respected as-is).
if not "%_OVSTAGE_SPHINXBUILD_DEFAULT%" == "1" goto check_sphinx
where uv >NUL 2>NUL
if errorlevel 1 goto check_sphinx
echo Using uv-managed environment from python\pyproject.toml [docs] extra...
REM --python 3.12 mirrors the published-site build (.github/workflows/docs.yml);
REM the interpreter version decides which Sphinx the resolver picks.
set SPHINXBUILD=uv run --python 3.12 --project ..\python --extra docs sphinx-build

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
	set _OVSTAGE_EXIT_CODE=1
	goto end
)

if "%1" == "" goto help

%SPHINXBUILD% -M %1 %SOURCEDIR% %BUILDDIR% %SPHINXOPTS% %O%
if errorlevel 1 goto error
goto end

:help
%SPHINXBUILD% -M help %SOURCEDIR% %BUILDDIR% %SPHINXOPTS% %O%
if errorlevel 1 goto error
goto end

:run_doxygen
echo Running Doxygen...
if exist "%DOXYGEN%" goto doxygen_found
where "%DOXYGEN%" >NUL 2>NUL
if not errorlevel 1 goto doxygen_found
echo Doxygen was not found. Install it with:
echo   winget install -e --id DimitriVanHeesch.Doxygen
echo Or download a project-local copy with:
echo   get-doxygen.bat
goto error
:doxygen_found
if not exist "_doxygen" mkdir "_doxygen"
if errorlevel 1 goto error
"%DOXYGEN%" Doxyfile
if errorlevel 1 goto error
if "%_OVSTAGE_BUILD_HTML%" == "1" goto setup_sphinx
goto end

:clean
if exist "%BUILDDIR%" rmdir /S /Q "%BUILDDIR%"
if errorlevel 1 goto error
if exist "_doxygen" rmdir /S /Q "_doxygen"
if errorlevel 1 goto error
goto end

:error
set _OVSTAGE_EXIT_CODE=%ERRORLEVEL%
if "%_OVSTAGE_EXIT_CODE%" == "0" set _OVSTAGE_EXIT_CODE=1

:end
popd
exit /b %_OVSTAGE_EXIT_CODE%
