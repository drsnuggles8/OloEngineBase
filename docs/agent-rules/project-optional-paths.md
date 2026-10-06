# Optional project paths

Preserve an empty asset-relative project field before joining it to the asset directory.
`ScriptModulePath` and `StartScene` use an empty path to mean unset.

The project loader previously joined an empty `ScriptModulePath` to `AssetDirectory`.
Build Game then treated the directory as a configured C# module. Requiring ScriptCore
for configured modules exposed this while verifying a project without a script module.
The same conversion also affected an empty start scene.

`RuntimeAssetPackTest.EmptyProjectAssetPathsRemainUnset` loads independent project YAML
and verifies both fields stay empty while the asset directory resolves normally.
The real missing-ScriptCore package control separately verifies that projects without
a configured module retain their optional assembly skip.
