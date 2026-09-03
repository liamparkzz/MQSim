Option Explicit

Dim shell
Dim fileSystem
Dim scriptDirectory
Dim syncScript
Dim gitExecutable
Dim powerShellExecutable
Dim command
Dim exitCode

If WScript.Arguments.Count < 1 Then
    WScript.Quit 2
End If

Set shell = CreateObject("WScript.Shell")
Set fileSystem = CreateObject("Scripting.FileSystemObject")

scriptDirectory = fileSystem.GetParentFolderName(WScript.ScriptFullName)
syncScript = fileSystem.BuildPath(scriptDirectory, "mqsim-auto-sync.ps1")
gitExecutable = WScript.Arguments(0)
powerShellExecutable = shell.ExpandEnvironmentStrings("%SystemRoot%") & "\System32\WindowsPowerShell\v1.0\powershell.exe"

command = QuoteArgument(powerShellExecutable) & _
    " -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " & QuoteArgument(syncScript) & _
    " -GitExecutable " & QuoteArgument(gitExecutable)

exitCode = shell.Run(command, 0, True)
WScript.Quit exitCode

Function QuoteArgument(ByVal value)
    QuoteArgument = Chr(34) & Replace(value, Chr(34), Chr(34) & Chr(34)) & Chr(34)
End Function
