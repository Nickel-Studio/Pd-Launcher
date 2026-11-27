## 改动

说明解决的问题、使用场景和改动范围。关联 Issue：

## 验证

- [ ] `python -B scripts/native/msvc.py -- cmake --list-presets`
- [ ] `python -B scripts/native/msvc.py -- cmake --build --preset native-core`
- [ ] `python -B scripts/native/msvc.py -- ctest --preset native-core`
- [ ] 涉及的回归用例已通过
- [ ] 文档、来源与许可证已同步

## 界面或兼容变化

涉及界面时附明暗模式截图；涉及旧配置、实例或便携包时说明兼容验证。
