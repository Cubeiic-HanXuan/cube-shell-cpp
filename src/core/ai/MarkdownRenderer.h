#pragma once

/**
 * @file MarkdownRenderer.h
 * @brief 将 Markdown 转成 Qt 富文本 HTML 片段，并按源文本块缓存转换结果。
 *
 * 本类为 AI 聊天内容提供轻量的格式转换工具：Markdown 语法由 QTextDocument
 * 内置解析器处理，本类负责分块、复用已渲染内容，以及提取可拼接的正文 HTML。
 * 输出面向 QTextBrowser 等 Qt 富文本控件，不包含完整 HTML 文档的 head/body 外壳。
 *
 * 使用 Qt 的 GitHub 风格 Markdown 方言，可解析标题、列表、表格、代码、链接等
 * 语法；同时禁用源文本中的原始 HTML，并阻止转换用的文档加载外部资源。
 * 这里不负责代码语法高亮、链接点击处理，也不生成自定义的语言 CSS 类。
 *
 * 对应早期 Python 实现：core/ai/ai_panel.py 中 AIChatPanel 的
 * _render_markdown、_build_table_html 和 _escape_html 等格式转换职责。
 */

#include <QCache>
#include <QString>
#include <QStringList>

namespace cubeshell {

/**
 * @brief 带块级 LRU 缓存的 Markdown 渲染器。
 *
 * render() 每次仍会扫描完整输入进行分块，但只对缓存未命中的块执行 Markdown
 * 解析。流式追加文本时，已经稳定且仍在缓存内的块可以直接复用；持续增长的
 * 尾块因源文本发生变化，会以新键重新解析。因此这是解析结果复用，不是仅接收
 * 增量字符的解析器，调用者每次应传入当前完整的 Markdown 文本。
 *
 * 每个块独立解析，跨块引用定义、被空行分开的列表等依赖上下文的语法，不保证
 * 与整篇一次性解析完全等价。需要整篇语法上下文时，可直接调用 renderBlock()。
 *
 * 缓存没有额外的并发保护，不应在多个线程中同时操作同一实例；渲染时还会读取
 * 当前 GUI 应用的默认字体，通常应在界面线程使用。
 *
 * @par 流式调用示例
 * @code{.cpp}
 * MarkdownRenderer renderer;
 * QString markdown = QStringLiteral("第一段\n\n第二");
 * const QString firstHtml = renderer.render(markdown);
 * markdown += QStringLiteral("段");
 * const QString nextHtml = renderer.render(markdown);
 * @endcode
 * 第二次调用时，“第一段\n\n”对应的块可命中缓存，“第二段”作为新块解析。
 * 两次返回值都是截至当次调用的完整正文；调用者应替换显示内容，不能把
 * nextHtml 当成追加片段接在 firstHtml 后面，否则会重复显示前面的段落。
 *
 * @par 生命周期与职责边界
 * 建议在连续更新期间复用同一实例；每次新建实例都会丢失已有块的解析结果。
 * 本类不保存“当前会话全文”，不负责刷新控件或记录消息，内部持久状态只有
 * 缓存。对象析构时，QCache 自动回收其持有的结果，无须手动逐条释放。
 */
class MarkdownRenderer {
public:
    /**
     * @brief 创建渲染器并设置缓存容量。
     * @param cacheBlocks 缓存的最大成本，默认 256；当前每个条目的成本为 1，
     *                    因而对应最多缓存的文本块数，而非字节数。
     *
     * 缓存采用 QCache 的最近最少使用淘汰策略。单个块长度不受此参数限制，
     * 所以它限制的是条目数量，不能作为严格的内存用量上限。
     * 传入 0 可使渲染结果不驻留缓存，仍正常执行转换；本类不额外校验或修正
     * 容量参数，调用者应传入非负值。
     */
    explicit MarkdownRenderer(int cacheBlocks = 256);

    /**
     * @brief 对完整 Markdown 文本分块渲染，并按原顺序拼接 HTML 片段。
     * @param markdown 当前完整文本；允许包含流式输出中尚未闭合的代码围栏。
     * @return 正文 HTML 片段；输入为空时返回空字符串。
     *
     * 缓存键是包含换行符在内的原始块文本。内容完全相同的块可以共享结果，
     * 无须由调用者维护块编号；清空或淘汰缓存只影响解析开销，不修改输入文本。
     * 除尾部追加外，也支持全文替换、回删或中间修改：每次都依据当前文本重新
     * 分块，旧内容仅可能留在缓存中，不会混入本次输出。返回空字符串不等于
     * 清空缓存；释放旧条目需调用 clearCache() 或等待容量淘汰。
     */
    QString render(const QString &markdown);

    /**
     * @brief 删除所有缓存条目，并释放 QCache 持有的 HTML 字符串。
     *
     * 缓存键只含文本，不含字体等渲染环境信息。若应用默认字体改变，调用者
     * 应清空缓存，以免旧块继续使用之前字体生成的 HTML。
     * 此操作保留缓存容量，且不影响之前已经返回的 HTML 字符串；它不会主动
     * 重绘界面，清空后需要重新调用渲染并将新结果交给展示控件。
     */
    void clearCache();

    // 以下辅助函数均不读取或写入实例缓存，便于单独使用和进行针对性测试。

    /**
     * @brief 将一段 Markdown 独立、全量解析为正文 HTML，不经过块缓存。
     * @param blockText 待解析文本；既可传入单块，也可传入需要统一上下文的整篇文本。
     * @return Qt 生成的 body 内部内容；空输入或无法提取正文时返回空字符串。
     *
     * 语法解析、代码内容保护以及未闭合围栏的处理均交给 QTextDocument。
     * 禁用原始 HTML 与资源加载的设置只作用于本次转换文档，不替代最终展示
     * 控件自身的资源加载策略和链接访问策略。
     * 输出保留 body 内部的标签及其属性，不保留 head 中的样式和 body 标签自身
     * 的属性；最终排版还取决于展示控件的默认样式，不应视作独立完整网页。
     * 只对长度为 0 的输入直接返回；纯空白文本仍交给 Qt 解析，不预先 trim。
     */
    static QString renderBlock(const QString &blockText);

    /**
     * @brief 转义普通文本中的 HTML 特殊字符，不进行 Markdown 解析。
     * @param text 需要作为普通文字放入 HTML 的原始文本。
     * @return 将 &、<、>、双引号及单引号转成实体后的文本；换行保持不变。
     *
     * 对应 Python 中 _escape_html 的字符转义部分，不包含换行转 <br> 的行为。
     * 本函数不检测已有实体，对已转义文本再次调用会继续转义其中的 &。
     */
    static QString escapeHtml(const QString &text);

    /**
     * @brief 按三反引号围栏和围栏外空行，将原文拆成可缓存的文本块。
     * @param markdown 完整 Markdown 源文本。
     * @return 按原顺序排列的块；空输入返回空列表，所有块直接拼接可还原原文。
     *
     * 每行保留原有换行符；围栏开始前先提交已有普通文本，围栏闭合时提交整个
     * 代码块，围栏外遇到空行则提交当前块。尚未闭合的围栏作为末尾块保留，
     * 便于流式输出继续追加后重新解析。
     *
     * 此处只是轻量的缓存边界扫描，并非完整 Markdown 语法解析：只检查去除
     * 首尾空白后是否以三个反引号开头，不匹配围栏长度，也不识别波浪号围栏。
     * 因此“拼接能还原原文”不代表“逐块渲染等价于整篇渲染”。
     *
     * @par 分块示例
     * 输入 "甲\n\n```cpp\nx\n\n```\n乙" 时，依次得到 "甲\n\n"、
     * "```cpp\nx\n\n```\n"、"乙" 三块，代码围栏内的空行不会切开第二块。
     * 连续的围栏外空行可以形成单独的空白块；只含空白的输入也不会在这里
     * 被丢弃。该保证针对原文字符，不保证富文本控件逐一显示这些空白。
     */
    static QStringList splitBlocks(const QString &markdown);

private:
    // 键为完整块源文本，值为堆上分配的正文 HTML 字符串。
    // insert() 使用默认成本 1；QCache 接管值指针，在淘汰、清空或析构时删除。
    // 缓存访问会更新最近使用状态，因此 render() 不是只读操作。
    QCache<QString, QString> m_cache;
};

} // namespace cubeshell
