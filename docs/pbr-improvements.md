# PBR Implementation Improvements TODO List

### Material System Refactoring
- [ ] Create IMaterial interface for polymorphic material handling
- [ ] Separate PBRMaterial and PhongMaterial classes
- [ ] Remove unsafe OpenGLShader* cast in Material::ConfigurePBRTextures()
- [ ] Implement proper material type enum and factory pattern
- [ ] Add material validation in constructors

### Performance Optimizations
- [ ] Batch texture bindings in Material::ConfigurePBRTextures()

### Shader System Enhancements
- [ ] Replace magic numbers with named constants
  - [ ] Create ShaderConstants.h with shared defines
  - [ ] Update all shaders to use constants
- [ ] Add multi-light support
  - [ ] Create LightBuffer UBO for multiple lights
  - [ ] Modify PBR.glsl to iterate through lights
  - [ ] Update Renderer3D to manage light arrays

### IBL System Improvements
- [ ] Add importance sampling for prefilter generation
- [ ] Implement configurable IBL resolution settings
- [ ] Add spherical harmonics option for diffuse irradiance
- [ ] Support HDR to LDR conversion with tone mapping options

### Error Handling
- [ ] Replace warnings with asserts for critical errors
- [ ] Add comprehensive error checking in IBL precomputation
- [ ] Implement material validation system
- [ ] Add shader compilation error reporting
