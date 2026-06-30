# AircraftPostProcessOutlineComponent 工作原理说明

本文结合 `AircraftPostProcessOutlineComponent.hpp/.cpp` 与相关 Shader，详细说明这个组件是如何完成“飞机后处理轮廓线”效果的。

相关文件：

- `component/AircraftPostProcessOutlineComponent.hpp`
- `component/AircraftPostProcessOutlineComponent.cpp`
- `shaders/aircraft_postprocess_mask.vert`
- `shaders/aircraft_postprocess_mask.frag`
- `shaders/aircraft_postprocess_outline.vert`
- `shaders/aircraft_postprocess_outline.frag`
- `animation/AirCombatScenario.cpp`

---

## 1. 这个组件解决的是什么问题

`AircraftPostProcessOutlineComponent` 实现的是一种**屏幕空间后处理描边**（post-process outline）。

和直接在线框、法线外扩壳体上做描边不同，它的思路是：

1. 先把“需要被描边的飞机/导弹/挂载模型”渲染到一个离屏缓冲里，得到目标的 mask 和目标深度；
2. 再从当前主场景帧缓冲中复制一份 scene depth；
3. 最后做一个全屏 quad pass，在片元着色器里根据 mask、scene depth、selected depth 计算边缘；
4. 把轮廓颜色以透明混合的方式叠加回当前画面。

这套方案的重点优点有两个：

- 轮廓线更像“最终视觉效果”，而不是调试线框；
- 可以做“遮挡感知”，也就是只给当前可见的目标外边缘描边，而不是隔着前方遮挡物也强行把后面的轮廓全画出来。

---

## 2. 对外接口与核心配置

头文件里先定义了两个关键数据结构。

### 2.1 描边模式枚举

```cpp
enum class AircraftPostProcessOutlineMode {
    DilateOnly,
    SobelOnly,
    DilateSobelHybrid,
    BlenderStyle
};
```

它表示 outline pass 里采用哪种边缘生成策略：

- `DilateOnly`
  - 纯膨胀，轮廓更厚、更柔和
- `SobelOnly`
  - 纯 Sobel，边更锐利，但一般更细
- `DilateSobelHybrid`
  - 膨胀和 Sobel 取 `max`，是当前默认模式
- `BlenderStyle`
  - 硬阈值的 Sobel 风格轮廓，更接近 DCC 工具里的清晰边缘线

### 2.2 运行配置

```cpp
struct AircraftPostProcessOutlineConfig {
    float thicknessPixels = 5.0f;
    bool occlusionAware = true;
    AircraftPostProcessOutlineMode mode = AircraftPostProcessOutlineMode::DilateSobelHybrid;
};
```

这三个参数决定了效果：

- `thicknessPixels`：描边厚度，单位是像素
- `occlusionAware`：是否启用遮挡感知
- `mode`：控制具体的边缘计算算法

### 2.3 `MatrixProvider` 的作用

```cpp
using MatrixProvider = std::function<glm::mat4()>;
```

这个设计允许组件在 draw 时为每个 mesh 单独获取自己的世界矩阵，而不是假设所有被描边 mesh 都共用同一个 `model` 矩阵。

因此组件可以支持：

- 单个 mesh
- 多个 mesh
- 多个来自不同 `SceneObject` 的 mesh
- 整个动态层级树

---

## 3. 组件的整体对象结构

`AircraftPostProcessOutlineComponent` 本身不是直接画 mesh，而是组装出一个特殊的 `RenderUnit`：

- `m_outlineRenderable`
  - 真正的核心逻辑在内部类 `AircraftPostProcessOutlineRenderable::draw(...)`
- `m_material`
  - 绑定 `aircraft_postprocess_outline` shader，用于最后的全屏 outline pass
- `m_renderUnit`
  - 被渲染系统收集并排序
- `m_modelMat`
  - 组件自己的变换矩阵，基本固定为单位矩阵
- `m_config`
  - 共享配置，供组件接口和内部 renderable 同时访问

所以这个类对外是一个普通 `Component`，但内部更像是一个把多阶段渲染流程封装起来的渲染任务。

---

## 4. 四种构造方式分别对应什么场景

### 4.1 单个 `MeshPtr`

```cpp
AircraftPostProcessOutlineComponent(const MeshPtr &sourceMesh, ...)
```

适合只给单个网格描边，默认使用当前 `RenderContext::model` 作为模型矩阵。

### 4.2 多个 `MeshPtr`

```cpp
AircraftPostProcessOutlineComponent(const std::vector<MeshPtr> &sourceMeshes, ...)
```

适合多个 mesh 共用外部上下文的 `model` 矩阵。

### 4.3 `Mesh + MatrixProvider`

```cpp
AircraftPostProcessOutlineComponent(
    const std::vector<std::pair<MeshPtr, MatrixProvider>> &sourcePairs, ...)
```

适合多个 mesh 分别来自不同对象，并且每个 mesh 需要在 draw 当帧获取自己的世界矩阵。

### 4.4 `SceneObjectWeakPtr` 根节点方式

```cpp
AircraftPostProcessOutlineComponent(const SceneObjectWeakPtr &rootObject, ...)
```

这是当前场景里最重要的一种方式。它不在构造时固定 mesh 列表，而是在每一帧 draw 时从 `rootObject` 开始递归遍历整棵 `SceneObject` 树，收集所有可见且有有效包围盒的 `Mesh` 以及对应 `worldMatrix`。

这样飞机机体、挂架、导弹等都能自动进入描边范围。

---

## 5. 初始化阶段做了什么

前三个构造函数的公共初始化逻辑集中在 `initOutlineComponent(...)` 中；`SceneObjectWeakPtr` 构造函数写了一份等价逻辑。

初始化阶段主要做了下面几件事。

### 5.1 构造共享配置

```cpp
outConfig = std::make_shared<AircraftPostProcessOutlineConfig>();
outConfig->thicknessPixels = std::max(kMinimumThicknessPixels, thicknessPixels);
outConfig->occlusionAware = true;
outConfig->mode = mode;
```

这里把厚度下限钳制到 `kMinimumThicknessPixels = 1.0f`，避免传入 0 或负值导致 shader 算法异常。

### 5.2 获取 outline shader 并创建材质

```cpp
auto shader = GResourceManager::getShaderProgram("aircraft_postprocess_outline");
```

随后创建 `Material` 并写入 uniform：

- `outlineColor`
- `outlineThicknessPixels`
- `outlineScreenParams`
- `outlineOcclusionAware`
- `outlineMode`

同时把材质设为透明并使用 Alpha 混合：

```cpp
outMaterial->setTransparent(true);
outMaterial->setBlendMode(BlendMode::Alpha);
```

原因是最终输出的轮廓不是完全覆盖屏幕，而是带 alpha 的颜色叠加。

### 5.3 创建自定义 renderable

```cpp
outRenderable = std::make_shared<AircraftPostProcessOutlineRenderable>(...);
```

后处理逻辑不在 `Component::update()` 里，而是在这个 `IRenderable` 的 `draw(...)` 中执行。

### 5.4 构造 `RenderUnit`

```cpp
outRenderUnit = RenderUnit(outRenderable,
                           outMaterial,
                           std::static_pointer_cast<const glm::mat4>(outModelMat));
outRenderUnit.layer = RenderLayer::Layer3D;
outRenderUnit.priority = kOutlineRenderPriority;
```

这里有两个要点：

- 它被放在 `Layer3D`
- 优先级固定为 `1600`

这意味着它会参与正常 3D 渲染排序，但通常比普通模型更靠后执行，以便在已有场景结果之上叠加轮廓。

---

## 6. 每帧 `update()` 做了什么

`update(float deltaTime)` 本身并不执行真正的后处理渲染，它只做轻量维护：

```cpp
*m_modelMat = glm::mat4(1.0f);
```

然后把最新参数重新同步到 `m_material`：

- `outlineColor`
- `outlineThicknessPixels`
- `outlineOcclusionAware`
- `outlineMode`

也就是说：

- `update()` 负责把外部状态灌进材质
- `draw()` 负责按这些状态执行真正的渲染流程

---

## 7. 渲染核心：`AircraftPostProcessOutlineRenderable::draw()`

真正的重点在内部类 `AircraftPostProcessOutlineRenderable` 的 `draw(const RenderContext&, const ShaderProgram&)`。

它可以拆成 6 个阶段理解。

---

## 8. 阶段一：收集本帧参与描边的 mesh

### 8.1 动态层级模式

如果构造时传的是 `SceneObjectWeakPtr rootObject`，就走：

```cpp
collectMeshesFromHierarchy(root, activeMeshes);
```

递归函数逻辑是：

1. 如果对象为空，或者对象不可见，直接返回
2. 取对象当前的 `worldMatrix`
3. 遍历对象的 `RenderUnit`
4. 把 `unit.renderable` 转成 `Mesh`
5. 如果转换成功且包围盒有效，则记入输出数组
6. 继续递归所有子节点

这样本帧的层级结构变化会自动反映出来，不需要在组件初始化后手工维护被描边 mesh 列表。

### 8.2 固定 mesh 列表模式

如果没有根对象，则直接遍历 `m_sourceMeshes`：

```cpp
for (const auto &[mesh, provider] : m_sourceMeshes) {
    if (!mesh) { continue; }
    activeMeshes.emplace_back(mesh, provider ? provider() : ctx.model);
}
```

关键点是：

- `provider()` 存在时，用它返回的世界矩阵
- 否则退回到当前渲染上下文的 `ctx.model`

---

## 9. 阶段二：准备离屏资源

### 9.1 自定义全屏 Quad

`FullscreenQuadRenderer` 是一个小工具类，用于在最后阶段绘制一个覆盖整个屏幕的矩形。

它负责：

- 创建 `VAO / VBO`
- 上传 4 个顶点
- 使用 `GL_TRIANGLE_STRIP` 画一个满屏四边形

顶点数据只有两部分：

- NDC 空间位置 `[-1,1]`
- 对应纹理坐标 `[0,1]`

### 9.2 `OutlineMaskFramebuffer` 的职责

`OutlineMaskFramebuffer` 是离屏缓冲管理器，内部维护：

- `m_fbo`
  - 掩码 pass 用的帧缓冲对象
- `m_maskTexture`
  - `GL_R8` 单通道纹理，存目标 mask
- `m_selectedDepthTexture`
  - 目标物体在 mask pass 中写出的深度
- `m_sceneDepthTexture`
  - 从当前主场景帧缓冲复制出来的深度

### 9.3 `ensureSize()` 的意义

```cpp
bool ensureSize(int width, int height)
```

它根据当前 viewport 尺寸确保离屏纹理大小正确。如果窗口大小改变，或者之前资源尚未建立，就会：

1. 释放旧资源
2. 新建 FBO
3. 新建 `maskTexture`
4. 新建 `selectedDepthTexture` 并挂到 `GL_DEPTH_ATTACHMENT`
5. 单独创建 `sceneDepthTexture`

注意：

- `maskTexture` 使用 `GL_R8`
- 两张深度纹理都用 `GL_DEPTH_COMPONENT24`
- 采样过滤都是 `GL_NEAREST`

这很适合后处理边缘检测。

---

## 10. 阶段三：保存当前 OpenGL 状态

在真正开始画之前，`draw()` 先把当前关键状态备份下来：

- 当前 draw/read framebuffer
- 当前 viewport
- 当前 active texture
- 当前深度测试、面剔除、混合是否开启
- 当前深度写掩码
- 当前剔除模式
- 当前 blend func

因为这个组件在执行时会临时修改大量 GL 状态；如果不恢复，后面的正常渲染对象就会被污染。

---

## 11. 阶段四：复制主场景深度

在进入 mask pass 前，代码先做：

```cpp
glBindFramebuffer(GL_READ_FRAMEBUFFER, previousDrawFramebuffer);
m_maskFramebuffer.copySceneDepthFromCurrentFramebuffer(viewportX, viewportY);
```

`copySceneDepthFromCurrentFramebuffer(...)` 内部通过 `glCopyTexSubImage2D(...)` 把当前主场景帧缓冲对应 viewport 范围内的深度拷贝到 `m_sceneDepthTexture`。

为什么一定要先拷贝？

因为后面的遮挡感知逻辑依赖于比较：

- 当前像素处目标物体的深度 `selectedDepth`
- 当前场景已经渲染出来的深度 `sceneDepth`

只有 `selectedDepth <= sceneDepth + epsilon` 时，才能认为这个目标像素在当前视图里是可见的。

---

## 12. 阶段五：mask pass

mask pass 的目标是：

- 在离屏 FBO 的颜色附件里得到目标区域 mask
- 在离屏 FBO 的深度附件里得到目标区域深度

核心代码是：

```cpp
m_maskFramebuffer.bindMaskPass();
glViewport(0, 0, viewportWidth, viewportHeight);
glDisable(GL_BLEND);
glEnable(GL_DEPTH_TEST);
glDepthMask(GL_TRUE);
glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
maskShader->use();
for (const auto &[mesh, worldMatrix] : activeMeshes) {
    RenderContext meshCtx = ctx;
    meshCtx.model = worldMatrix;
    mesh->draw(meshCtx, *maskShader);
}
```

### 12.1 为什么这里要开启深度测试

因为目标对象本身也可能有前后遮挡关系。如果不让 mask pass 正常写深度，那么得到的 `selectedDepthTexture` 就不可信，后续遮挡判断和轮廓位置都会出问题。

### 12.2 mask shader 非常简单

顶点着色器：

```glsl
uniform mat4 mvpMat;
layout(location = 0) in vec3 vertexPosition;
void main()
{
    gl_Position = mvpMat * vec4(vertexPosition, 1.0);
}
```

片元着色器：

```glsl
layout(location = 0) out vec4 fragColor;
void main()
{
    fragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
```

虽然输出的是红色，但由于 `m_maskTexture` 是 `GL_R8` 单通道纹理，真正有意义的是红色通道为 1。于是最终效果就是：

- 目标区域写入 `mask = 1`
- 其它区域保持 `mask = 0`

同时深度附件里记录下目标最近可见表面的深度。

---

## 13. 阶段六：outline pass（全屏后处理）

mask pass 完成后，组件恢复回原始 framebuffer，并开始真正的全屏描边阶段。

关键设置包括：

```cpp
glDisable(GL_DEPTH_TEST);
glDepthMask(GL_FALSE);
glDisable(GL_CULL_FACE);
```

因为现在要画的是屏幕 quad，而不是 3D 几何体。

随后绑定三个输入纹理：

- `maskTexture` -> 纹理单元 0
- `sceneDepthTexture` -> 纹理单元 1
- `selectedDepthTexture` -> 纹理单元 2

并写入：

```cpp
shader.setUniform("outlineScreenParams",
                  glm::vec4(viewportWidth,
                            viewportHeight,
                            1.0f / viewportWidth,
                            1.0f / viewportHeight));
```

含义是：

- `xy`：屏幕尺寸
- `zw`：一个像素对应的 UV 步长

最后：

```cpp
m_fullscreenQuad.draw();
```

于是全屏每个像素都会运行一次 `aircraft_postprocess_outline.frag`。

---

## 14. outline fragment shader 是如何计算轮廓的

这是整个效果最核心的部分。

### 14.1 第一步：判断当前像素是不是目标自身可见区域

```glsl
if (sampleVisibleMask(texCoord) > 0.5) {
    fragColor = vec4(0.0);
    return;
}
```

这表示：

- 如果当前像素本身就在目标可见区域内
- 那就不要画轮廓
- 轮廓只画在目标外侧

### 14.2 `sampleVisibleMask` 的逻辑

`sampleVisibleMask(vec2 uv)` 先看 `maskTexture`：

```glsl
if (texture(maskTexture, clampedUv).r < 0.5) {
    return 0.0;
}
```

如果该点不属于目标区域，直接返回 0。

如果属于目标区域：

- 当 `outlineOcclusionAware == 0`，直接返回 1
- 当 `outlineOcclusionAware != 0`，继续比较深度：

```glsl
float selectedDepth = texture(selectedDepthTexture, clampedUv).r;
float sceneDepth = texture(sceneDepthTexture, clampedUv).r;
return selectedDepth <= (sceneDepth + kDepthEpsilon) ? 1.0 : 0.0;
```

也就是说，遮挡感知不是发生在最后输出颜色那一步，而是发生在轮廓采样源的定义这一步。

---

## 15. 三种边缘计算函数分别做什么

### 15.1 `computeDilateWeight`

它在一个正方形邻域里遍历采样点，邻域半径由：

```glsl
int radius = clamp(int(ceil(outlineThicknessPixels)), 1, kMaxKernelRadius);
```

决定。

它会寻找邻域内是否存在可见目标 mask，如果存在，就按距离给当前像素一个权重：

- 离目标越近，权重越大
- 离目标越远，权重越小
- 超出半径则不算

结果是得到一种向外膨胀的边缘带，比较适合生成较粗的轮廓。

### 15.2 `computeSobelWeight`

它对周围 8 个邻点做经典 Sobel 梯度计算：

- `gx` 检测 x 方向变化
- `gy` 检测 y 方向变化
- 最后取 `length(vec2(gx, gy))`

由于输入是 0/1 的可见 mask，所以 Sobel 实际上是在检测目标区域的边界跳变。

### 15.3 `computeBlenderStyleWeight`

这个函数本质上也是 Sobel，但它不输出渐变权重，而是做硬阈值：

```glsl
return length(vec2(gx, gy)) > 0.1 ? 1.0 : 0.0;
```

所以它的特征是：

- 边缘二值化
- 不带柔和过渡
- 视觉上更硬、更利落

---

## 16. 为什么 `Hybrid` 模式通常是默认最稳妥的

在默认分支里：

```glsl
float sobelWeight = computeSobelWeight(texCoord, texelStep);
float dilateWeight = computeDilateWeight(texCoord, radius, texelStep);
outlineAlpha = max(dilateWeight, sobelWeight);
```

这表示：

- Sobel 提供清晰边缘定位
- Dilate 提供厚度与饱满度
- 两者取最大值，既不容易太虚，也不容易太细

---

## 17. `thicknessPixels` 是如何生效的

组件里 thickness 会在两处被约束：

1. C++ 配置层面最小限制为 `1.0f`
2. 真正传给 shader 时还会再钳制到 `kMaxOutlineKernelRadiusPixels = 8`

这么做主要是为了控制 shader 的最坏采样成本，因为 `computeDilateWeight` 是一个双重循环。

---

## 18. 为什么这个组件要自己管理 GL 状态恢复

在 outline pass 之后，代码会把以下状态恢复回去：

- 深度测试开关
- 深度写掩码
- 面剔除开关与剔除模式
- read/draw framebuffer
- active texture
- blend func
- viewport

因为该组件是通过在当前渲染流程中插入一次离屏 + 全屏 pass 工作的。如果它不显式恢复状态，后续对象就会被污染。

---

## 19. `setEnabled()`、`update()` 与 `draw()` 三者如何配合

### 19.1 `setEnabled()`

`Component` 基类的 `setEnabled()` 会调用 `onSetEnabled(bool enabled)`。本组件在 `onSetEnabled()` 中做的是：

```cpp
if (m_outlineRenderable != nullptr) {
    m_outlineRenderable->setEnabled(enabled);
}
```

### 19.2 `update()`

负责同步运行参数，保证材质 uniform 和当前组件状态一致。

### 19.3 `draw()`

只有在渲染阶段、并且 renderable 启用时才真正执行多阶段后处理。

---

## 20. 在场景中是如何接入的

`AirCombatScenario.cpp` 里，飞机轮廓的接入点是 `attachAircraftOutline(...)`。

当轮廓技术选择为 `PostProcessScreenSpace` 时：

```cpp
auto outlineComponent = std::make_shared<AircraftPostProcessOutlineComponent>(
    SceneObjectWeakPtr{aircraftObject},
    outlineColor,
    kAircraftPostProcessOutlineThicknessPixels);
outlineComponent->setOcclusionAware(kAircraftPostProcessOcclusionAware);
outlineComponent->setOutlineMode(kAircraftPostProcessOutlineMode);
aircraftObject->addComponent(outlineComponent);
```

这里直接传入飞机对象根节点，所以后续挂到飞机子节点上的内容也能自动进入 outline。

---

## 21. 这个实现的优点

### 21.1 支持复杂层级对象

动态遍历 `SceneObject` 树的设计很适合飞机 + 导弹 + 挂架 + 子部件这种组合对象。

### 21.2 可做遮挡感知

通过比较 `selectedDepthTexture` 和 `sceneDepthTexture`，只对当前可见目标参与描边，视觉效果更自然。

### 21.3 边缘算法可切换

支持 `DilateOnly`、`SobelOnly`、`DilateSobelHybrid`、`BlenderStyle` 四种模式，方便针对不同美术需求调节效果。

### 21.4 与模型拓扑解耦

它不需要修改原始 mesh，也不依赖法线外扩，因此对于复杂模型、共享模型、动态挂载都比较友好。

---

## 22. 这个实现的代价与注意点

### 22.1 额外的离屏开销

每次描边都会有：

- 一次场景深度复制
- 一次目标 mask pass
- 一次全屏 pass

### 22.2 `Dilate` 模式成本和厚度相关

`computeDilateWeight` 的采样成本随着半径变大而增大，因此不能把 `thicknessPixels` 无限制放大。

### 22.3 依赖当前帧缓冲已有深度

遮挡感知成立的前提是主场景深度已经正确写入，并且当前 outline 组件执行时，前面该有的内容已经画完。

### 22.4 只适合描整体轮廓感

它主要强调物体外轮廓，并不是用来表达网格内部结构线。

---

## 23. 用一句话概括它的工作流程

`AircraftPostProcessOutlineComponent` 的本质是：

> 每帧收集目标飞机层级中的所有 mesh，先离屏生成目标 mask 与目标深度，再结合主场景深度做遮挡判定，最后通过全屏 shader 在目标可见边界外侧生成并叠加轮廓线。

---

## 24. 简化时序图

可以把它抽象成下面这条时序：

1. 场景正常渲染到当前 framebuffer
2. 组件 draw 开始
3. 复制当前 framebuffer 深度到 `sceneDepthTexture`
4. 将目标 mesh 渲染到离屏 FBO：
   - `maskTexture`
   - `selectedDepthTexture`
5. 切回原 framebuffer
6. 绑定 outline shader
7. 全屏 quad 采样三张纹理并计算 `outlineAlpha`
8. 将 `outlineColor * alpha` 混合回画面
9. 恢复 OpenGL 状态
10. 后续渲染继续

---

## 25. 总结

从工程实现上看，`AircraftPostProcessOutlineComponent.cpp` 的核心价值在于三点：

1. **数据源组织灵活**：既支持固定 mesh，也支持动态层级遍历
2. **视觉上更成熟**：通过屏幕空间后处理获得稳定外轮廓
3. **工程上可控**：通过共享配置、材质 uniform、GL 状态保存与恢复，把一套复杂渲染流程封装成普通组件接口

如果用一句工程语言概括：

> 这是一个把“目标收集、离屏 mask、深度比较、全屏边缘检测、结果叠加”完整打包起来的飞机轮廓后处理组件。
