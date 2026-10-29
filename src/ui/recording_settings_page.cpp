#include "ui/recording_settings_page.h"
#include "app/history_store.h"
#include "core/storage_directory.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
namespace waibusnap
{
RecordingSettingsPage::RecordingSettingsPage(RecordingSettingsActions actions, QWidget* parent)
    : QWidget(parent), actions_(std::move(actions))
{
    setObjectName(QStringLiteral("settingsRecordingPage"));
    const auto& p = actions_.preferences;
    auto* layout = new QVBoxLayout(this);
    if (!actions_.supported)
    {
        auto* notice = new QLabel(
            QStringLiteral(
                "录制将在 Windows 原生采集补齐后提供；已有录制文件仍可在资源浏览器中浏览。"),
            this);
        notice->setObjectName(QStringLiteral("recordingUnsupportedNotice"));
        notice->setWordWrap(true);
        layout->addWidget(notice);
    }
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    auto* body = new QVBoxLayout(content);
    const auto group = [body, content](const QString& title)
    {
        auto* box = new QGroupBox(title, content);
        body->addWidget(box);
        auto* form = new QFormLayout(box);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        return form;
    };
    const auto combo =
        [this](const char* name, const QStringList& labels, const QList<int>& values, int current)
    {
        auto* control = new QComboBox(this);
        control->setObjectName(QLatin1String(name));
        for (qsizetype i = 0; i < labels.size(); ++i)
            control->addItem(labels[i], values[i]);
        control->setCurrentIndex(qMax(0, control->findData(current)));
        connect(control, &QComboBox::currentIndexChanged, this, [this] { updateState(); });
        return control;
    };
    auto* output = group(QStringLiteral("输出"));
    auto* outputs = new QHBoxLayout;
    video_ = new QRadioButton(QStringLiteral("视频"), this);
    video_->setObjectName(QStringLiteral("recordingDefaultVideo"));
    gif_ = new QRadioButton(QStringLiteral("GIF"), this);
    gif_->setObjectName(QStringLiteral("recordingDefaultGif"));
    auto* buttons = new QButtonGroup(this);
    buttons->addButton(video_);
    buttons->addButton(gif_);
    video_->setChecked(p.defaultOutput == RecordingOutput::Video);
    gif_->setChecked(p.defaultOutput == RecordingOutput::Gif);
    outputs->addWidget(video_);
    outputs->addWidget(gif_);
    outputs->addStretch();
    output->addRow(QStringLiteral("默认输出"), outputs);
    auto* video = group(QStringLiteral("视频"));
    video->addRow(QStringLiteral("格式"), new QLabel(QStringLiteral("MP4 / H.264（固定）"), this));
    resolution_ = combo(
        "recordingResolution",
        {QStringLiteral("原尺寸（物理像素）"), QStringLiteral("1080p"), QStringLiteral("720p")},
        {0, 1, 2}, int(p.resolution));
    video->addRow(QStringLiteral("分辨率"), resolution_);
    videoFps_ = combo("recordingVideoFps",
                      {QStringLiteral("30 帧"), QStringLiteral("60 帧（更高性能与体积）")},
                      {30, 60}, p.videoFps);
    video->addRow(QStringLiteral("帧率"), videoFps_);
    quality_ = combo("recordingQuality",
                     {QStringLiteral("高"), QStringLiteral("中"), QStringLiteral("低")}, {0, 1, 2},
                     int(p.quality));
    video->addRow(QStringLiteral("质量"), quality_);
    videoEstimate_ = new QLabel(this);
    videoEstimate_->setObjectName(QStringLiteral("recordingVideoEstimate"));
    videoEstimate_->setWordWrap(true);
    video->addRow(QStringLiteral("估算"), videoEstimate_);
    auto* gif = group(QStringLiteral("GIF"));
    gifEdge_ = combo("recordingGifMaxEdge",
                     {QStringLiteral("960"), QStringLiteral("1280"), QStringLiteral("原尺寸")},
                     {960, 1280, 0}, p.gifMaxEdge);
    gifFps_ = combo("recordingGifFps",
                    {QStringLiteral("10 帧"), QStringLiteral("12 帧"), QStringLiteral("15 帧")},
                    {10, 12, 15}, p.gifFps);
    gifSeconds_ = combo("recordingGifMaxSeconds",
                        {QStringLiteral("15 秒"), QStringLiteral("30 秒"), QStringLiteral("60 秒")},
                        {15, 30, 60}, p.gifMaxSeconds);
    gif->addRow(QStringLiteral("最长边"), gifEdge_);
    gif->addRow(QStringLiteral("帧率"), gifFps_);
    gif->addRow(QStringLiteral("最长时长"), gifSeconds_);
    gif->addRow(QStringLiteral("循环"), new QLabel(QStringLiteral("无限（固定）"), this));
    gifEstimate_ = new QLabel(this);
    gifEstimate_->setObjectName(QStringLiteral("recordingGifEstimate"));
    gifEstimate_->setWordWrap(true);
    gif->addRow(QStringLiteral("估算"), gifEstimate_);
    auto* directories = group(QStringLiteral("目录"));
    if (!actions_.chooseDirectory)
        actions_.chooseDirectory = [](QWidget* parent, const QString& current) {
            return QFileDialog::getExistingDirectory(parent, QStringLiteral("选择录制目录"),
                                                     current);
        };
    for (bool isGif : {false, true})
    {
        auto* row = new QHBoxLayout;
        auto*& field = isGif ? gifPath_ : videoPath_;
        field = new QLineEdit(this);
        field->setReadOnly(true);
        field->setObjectName(isGif ? QStringLiteral("recordingGifDirectory")
                                   : QStringLiteral("recordingVideoDirectory"));
        auto* choose = new QPushButton(QStringLiteral("选择…"), this);
        choose->setObjectName(isGif ? QStringLiteral("recordingChooseGifDirectory")
                                    : QStringLiteral("recordingChooseVideoDirectory"));
        auto* restore = new QPushButton(QStringLiteral("恢复默认"), this);
        restore->setObjectName(isGif ? QStringLiteral("recordingRestoreGifDirectory")
                                     : QStringLiteral("recordingRestoreVideoDirectory"));
        row->addWidget(field, 1);
        row->addWidget(choose);
        row->addWidget(restore);
        directories->addRow(isGif ? QStringLiteral("GIF 目录") : QStringLiteral("视频目录"), row);
        setDirectory(isGif, isGif ? p.gifDirectory : p.videoDirectory);
        connect(choose, &QPushButton::clicked, this,
                [this, isGif]
                {
                    const auto path = actions_.chooseDirectory(this, directory(isGif));
                    if (!path.isEmpty())
                        setDirectory(isGif, path);
                });
        connect(restore, &QPushButton::clicked, this, [this, isGif] { setDirectory(isGif, {}); });
    }
    directoryError_ = new QLabel(this);
    directoryError_->setObjectName(QStringLiteral("recordingDirectoryError"));
    directoryError_->setWordWrap(true);
    directoryError_->setStyleSheet(QStringLiteral("color: #d63c36;"));
    directoryError_->hide();
    directories->addRow(directoryError_);
    auto* general = group(QStringLiteral("通用"));
    countdown_ = combo("recordingCountdown",
                       {QStringLiteral("0 秒"), QStringLiteral("3 秒"), QStringLiteral("5 秒")},
                       {0, 3, 5}, p.countdownSeconds);
    general->addRow(QStringLiteral("倒计时"), countdown_);
    countdownHelp_ = new QLabel(QStringLiteral("不显示倒计时，立即开始"), this);
    countdownHelp_->setObjectName(QStringLiteral("recordingCountdownHelp"));
    general->addRow(countdownHelp_);
    cursor_ = new QCheckBox(QStringLiteral("录制中显示光标"), this);
    cursor_->setObjectName(QStringLiteral("recordingShowCursor"));
    cursor_->setChecked(p.showCursor);
    cursor_->setToolTip(QStringLiteral("关闭后成片不显示鼠标指针"));
    general->addRow(cursor_);
    auto* audioForm = group(QStringLiteral("音频"));
    audio_ = qobject_cast<QGroupBox*>(audioForm->parentWidget());
    audio_->setObjectName(QStringLiteral("recordingAudioGroup"));
    systemAudio_ = new QCheckBox(QStringLiteral("录制系统声音"), this);
    systemAudio_->setObjectName(QStringLiteral("recordingSystemAudio"));
    systemAudio_->setChecked(p.systemAudio);
    audioForm->addRow(systemAudio_);
    const auto volume = [this, audioForm](const char* spinName, const char* sliderName, int current,
                                          QSlider*& slider)
    {
        auto* row = new QHBoxLayout;
        auto* spin = new QSpinBox(this);
        spin->setObjectName(QLatin1String(spinName));
        spin->setRange(0, 200);
        spin->setSuffix(QStringLiteral(" %"));
        spin->setValue(current);
        slider = new QSlider(Qt::Horizontal, this);
        slider->setObjectName(QLatin1String(sliderName));
        slider->setRange(0, 200);
        slider->setValue(current);
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
        row->addWidget(slider, 1);
        row->addWidget(spin);
        audioForm->addRow(QStringLiteral("音量"), row);
        return spin;
    };
    systemVolume_ = volume("recordingSystemVolume", "recordingSystemVolumeSlider",
                           p.systemVolumePercent, systemSlider_);
    microphone_ = new QCheckBox(QStringLiteral("录制麦克风"), this);
    microphone_->setObjectName(QStringLiteral("recordingMicrophone"));
    microphone_->setChecked(p.microphone);
    audioForm->addRow(microphone_);
    auto* devices = new QHBoxLayout;
    devices_ = new QComboBox(this);
    devices_->setObjectName(QStringLiteral("recordingMicrophoneDevice"));
    devices_->addItem(QStringLiteral("系统默认输入"), QString());
    if (!p.microphoneDeviceId.isEmpty())
    {
        devices_->addItem(QStringLiteral("已选设备（等待刷新）"), p.microphoneDeviceId);
        devices_->setCurrentIndex(1);
    }
    refresh_ = new QPushButton(QStringLiteral("刷新"), this);
    refresh_->setObjectName(QStringLiteral("recordingRefreshMicrophones"));
    devices->addWidget(devices_, 1);
    devices->addWidget(refresh_);
    audioForm->addRow(QStringLiteral("设备"), devices);
    microphoneVolume_ = volume("recordingMicrophoneVolume", "recordingMicrophoneVolumeSlider",
                               p.microphoneVolumePercent, microphoneSlider_);
    audioHelp_ = new QLabel(this);
    audioHelp_->setObjectName(QStringLiteral("recordingAudioHelp"));
    audioHelp_->setWordWrap(true);
    body->addWidget(audioHelp_);
    body->addWidget(
        new QLabel(QStringLiteral("设置仅影响下一次录制；麦克风首次录制时请求授权。"), this));
    body->addStretch();
    connect(video_, &QRadioButton::toggled, this, [this] { updateState(); });
    connect(microphone_, &QCheckBox::toggled, this, [this] { updateState(); });
    connect(systemAudio_, &QCheckBox::toggled, this, [this] { updateState(); });
    connect(refresh_, &QPushButton::clicked, this, [this] { refreshMicrophones(); });
    updateState();
    content->setEnabled(actions_.supported);
}
QString RecordingSettingsPage::directory(bool gif) const
{
    const auto path = gif ? gifDirectory_ : videoDirectory_;
    return path.isEmpty()
               ? (gif ? defaultRecordingGifDirectory() : defaultRecordingVideoDirectory())
               : path;
}
void RecordingSettingsPage::setDirectory(bool gif, const QString& path)
{
    (gif ? gifDirectory_ : videoDirectory_) = path;
    auto* field = gif ? gifPath_ : videoPath_;
    const auto absolute = directory(gif);
    const auto home = QDir::homePath();
    field->setText(absolute.startsWith(home + QLatin1Char('/'))
                       ? QStringLiteral("~") + absolute.mid(home.size())
                       : absolute);
    field->setToolTip(absolute);
}
RecordingPreferences RecordingSettingsPage::collect() const
{
    auto p = actions_.preferences;
    p.videoDirectory = videoDirectory_;
    p.gifDirectory = gifDirectory_;
    p.defaultOutput = gif_->isChecked() ? RecordingOutput::Gif : RecordingOutput::Video;
    p.resolution = RecordingResolution(resolution_->currentData().toInt());
    p.videoFps = videoFps_->currentData().toInt();
    p.quality = RecordingQuality(quality_->currentData().toInt());
    p.gifMaxEdge = gifEdge_->currentData().toInt();
    p.gifFps = gifFps_->currentData().toInt();
    p.gifMaxSeconds = gifSeconds_->currentData().toInt();
    p.countdownSeconds = countdown_->currentData().toInt();
    p.showCursor = cursor_->isChecked();
    p.systemAudio = systemAudio_->isChecked();
    p.microphone = microphone_->isChecked();
    p.microphoneDeviceId = devices_->currentData().toString();
    p.systemVolumePercent = systemVolume_->value();
    p.microphoneVolumePercent = microphoneVolume_->value();
    return p;
}
void RecordingSettingsPage::updateState()
{
    // 构造期间 combo 的信号可能先于其他字段创建。
    if (!audio_ || !microphoneVolume_)
        return;
    const auto p = collect();
    const bool video = p.defaultOutput == RecordingOutput::Video;
    audio_->setEnabled(video);
    devices_->setEnabled(p.microphone);
    microphoneVolume_->setEnabled(p.microphone);
    microphoneSlider_->setEnabled(p.microphone);
    systemVolume_->setEnabled(p.systemAudio);
    systemSlider_->setEnabled(p.systemAudio);
    audioHelp_->setText(video ? QStringLiteral("麦克风需要系统授权；关闭系统声音后不写系统音轨。")
                              : QStringLiteral("GIF 不含声音；切回视频后生效"));
    countdownHelp_->setVisible(p.countdownSeconds == 0);
    QSize source(1920, 1080);
    if (const auto* screen = QGuiApplication::primaryScreen())
        source = QSize(qRound(screen->size().width() * screen->devicePixelRatio()),
                       qRound(screen->size().height() * screen->devicePixelRatio()));
    const auto pixels =
        recordingOutputSize(source, RecordingOutput::Video, p.resolution, p.gifMaxEdge);
    videoEstimate_->setText(
        QStringLiteral("%1%2 %3 约 %4 Mbps；估算值，实际以编码结果为准。原尺寸以主屏为参考。")
            .arg(p.resolution == RecordingResolution::Native ? QStringLiteral("原尺寸")
                                                             : resolution_->currentText())
            .arg(p.videoFps)
            .arg(quality_->currentText())
            .arg(estimatedVideoBitrate(pixels, p.videoFps, p.quality) / 1000000.0, 0, 'f', 1));
    const auto gifPixels =
        recordingOutputSize(source, RecordingOutput::Gif, p.resolution, p.gifMaxEdge);
    gifEstimate_->setText(
        QStringLiteral("30 秒约 %1 MB；单次最长 %2 "
                       "秒。估算值，实际以编码结果为准（按主屏比例参考；内容复杂时更大）。")
            .arg(estimatedGifBytes(gifPixels, p.gifFps, 30) / 1000000.0, 0, 'f', 1)
            .arg(p.gifMaxSeconds));
}
void RecordingSettingsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!devicesRequested_ && actions_.supported)
        refreshMicrophones();
}
void RecordingSettingsPage::refreshMicrophones()
{
    if (!actions_.supported || refreshing_ || !actions_.queryMicrophones)
        return;
    devicesRequested_ = true;
    refreshing_ = true;
    refresh_->setEnabled(false);
    const auto query = actions_.queryMicrophones;
    const QPointer<RecordingSettingsPage> guard(this);
    QThreadPool::globalInstance()->start(
        [query, guard]
        {
            const auto devices = query();
            QMetaObject::invokeMethod(
                qApp,
                [guard, devices]
                {
                    if (!guard)
                        return;
                    const auto selected = guard->devices_->currentData().toString();
                    guard->devices_->clear();
                    guard->devices_->addItem(QStringLiteral("系统默认输入"), QString());
                    for (const auto& device : devices)
                        if (!device.id.isEmpty())
                            guard->devices_->addItem(device.name, device.id);
                    int index = guard->devices_->findData(selected);
                    if (index < 0)
                    {
                        guard->devices_->addItem(QStringLiteral("已选设备（当前不可用）"),
                                                 selected);
                        index = guard->devices_->count() - 1;
                    }
                    guard->devices_->setCurrentIndex(index);
                    guard->refreshing_ = false;
                    guard->refresh_->setEnabled(true);
                },
                Qt::QueuedConnection);
        });
}
QString RecordingSettingsPage::prepareDirectories(const QString& historyRoot, QStringList groups)
{
    directoryError_->hide();
    const auto reject = [this](const QString& error)
    {
        directoryError_->setText(error);
        directoryError_->show();
        QTimer::singleShot(0, this, [this]
                           { findChild<QScrollArea*>()->ensureWidgetVisible(directoryError_); });
        return error;
    };
    QString quick, error;
    if (actions_.reservedDirectories)
        groups.append(actions_.reservedDirectories(historyRoot, &quick, &error));
    if (!error.isEmpty())
        return reject(error);
    groups.prepend(historyRoot.isEmpty() ? HistoryStore::defaultRootDirectory() : historyRoot);
    QStringList paths;
    for (bool gif : {false, true})
    {
        const auto path = directory(gif);
        const auto label = gif ? QStringLiteral("GIF 目录") : QStringLiteral("视频目录");
        if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null))
            return reject(label + QStringLiteral("必须为绝对路径且不能包含空字符。"));
        const auto canonical = canonicalStorageDirectory(path);
        if (canonical.isEmpty())
            return reject(label + QStringLiteral("无法创建或解析，请选择有效目录。"));
        for (const auto& group : groups)
        {
            if (group.isEmpty())
                continue;
            const auto other = canonicalStorageDirectory(group);
            if (other.isEmpty())
                return reject(QStringLiteral("无法解析历史根目录或分组目录，请检查后重试。"));
            // 录制目录可在历史根 / 分组之下；相等或包含历史目录会影响整个来源，拒绝。
            if (storageDirectoryContains(canonical, other))
                return reject(label + QStringLiteral("不能等于或包含历史根目录、分组目录。"));
        }
        if (!quick.isEmpty())
        {
            const auto other = canonicalStorageDirectory(quick);
            if (other.isEmpty() || storageDirectoriesOverlap(canonical, other))
                return reject(label + QStringLiteral("不能与快枪手目录相同或互相包含。"));
        }
        paths.append(canonical);
    }
    if (storageDirectoriesOverlap(paths[0], paths[1]))
        return reject(QStringLiteral("视频目录与 GIF 目录不能相同或互相包含。"));
    for (const auto& path : paths)
    {
        if (!QDir().mkpath(path))
            return reject(QStringLiteral("录制目录创建失败：%1").arg(path));
        QTemporaryFile probe(
            QDir(path).filePath(QStringLiteral(".waibusnap-recording-check-XXXXXX")));
        if (!QFileInfo(path).isWritable() || !probe.open() || probe.write("probe", 5) != 5 ||
            !probe.flush())
            return reject(QStringLiteral("录制目录不可写：%1").arg(path));
        if (!probe.remove())
            return reject(QStringLiteral("录制目录写入探针清理失败：%1").arg(path));
    }
    return {};
}
}
