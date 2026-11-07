' Double-click launcher for the OTA one-click tool. No console window.
' Right-click -> Send to -> Desktop (create shortcut) for one-click start.
' Advanced full UI is reachable from the "advanced mode" button in the simple window.
'
' pythonw.exe is resolved automatically:
'   1) the python.exe on PATH (same folder, pythonw.exe);
'   2) a few common Conda / Python install locations, built from %USERPROFILE%,
'      %LOCALAPPDATA% and %ProgramData% so no machine-specific path is baked in.
Option Explicit

Dim fso, sh, here, py, cmd, execObj, line, dir, i, p, candidates

Set fso = CreateObject("Scripting.FileSystemObject")
Set sh  = CreateObject("WScript.Shell")
here = fso.GetParentFolderName(WScript.ScriptFullName)
py = ""

' ---- 1) derive pythonw.exe from the python.exe found on PATH -----------------
Set execObj = sh.Exec("%ComSpec% /c where python 2>nul")
Do While Not execObj.StdOut.AtEndOfStream
    line = Trim(execObj.StdOut.ReadLine())
    If Len(line) > 0 Then
        dir = fso.GetParentFolderName(line)
        If fso.FileExists(dir & "\pythonw.exe") Then
            py = dir & "\pythonw.exe"
            Exit Do
        End If
    End If
Loop

' ---- 2) common install locations ---------------------------------------------
If py = "" Then
    candidates = Array( _
        "%USERPROFILE%\anaconda3\pythonw.exe", _
        "%USERPROFILE%\miniconda3\pythonw.exe", _
        "%LOCALAPPDATA%\Programs\Python\Python313\pythonw.exe", _
        "%LOCALAPPDATA%\Programs\Python\Python312\pythonw.exe", _
        "%LOCALAPPDATA%\Programs\Python\Python311\pythonw.exe", _
        "%ProgramData%\anaconda3\pythonw.exe", _
        "%ProgramData%\miniconda3\pythonw.exe")
    For i = 0 To UBound(candidates)
        p = sh.ExpandEnvironmentStrings(candidates(i))
        If fso.FileExists(p) Then
            py = p
            Exit For
        End If
    Next
End If

' ---- 3) last resort: let Windows resolve it ----------------------------------
If py = "" Then py = "pythonw.exe"

sh.CurrentDirectory = here
cmd = """" & py & """ """ & here & "\quick_ota.py"""
sh.Run cmd, 0, False
