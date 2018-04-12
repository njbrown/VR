#include "projectmanager.h"
#include "ui_projectmanager.h"

#include <chrono>
#include <memory>

#include <QtConcurrent/QtConcurrent>
#include <QDebug>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGraphicsDropShadowEffect>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QThread>
#include <QTreeWidgetItem>
#include <QStyledItemDelegate>

#include "irisgl/src/assimp/include/assimp/Importer.hpp"
#include "irisgl/src/core/irisutils.h"
#include "irisgl/src/materials/custommaterial.h"
#include "irisgl/src/zip/zip.h"

#include "constants.h"
#include "dynamicgrid.h"
#include "globals.h"
#include "itemgridwidget.hpp"
#include "mainwindow.h"
#include "uimanager.h"

#include "core/database/database.h"
#include "core/guidmanager.h"
#include "core/thumbnailmanager.h"
#include "core/project.h"
#include "core/settingsmanager.h"
#include "dialogs/newprojectdialog.h"
#include "dialogs/progressdialog.h"
#include "io/assetmanager.h"

ProjectManager::ProjectManager(Database *handle, QWidget *parent) : QWidget(parent), ui(new Ui::ProjectManager)
{
    ui->setupUi(this);
    db = handle;

#ifdef Q_OS_WIN32
	// setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NativeWindow, true);
#endif

	futureWatcher = QPointer<QFutureWatcher<QVector<ModelData>>>(new QFutureWatcher<QVector<ModelData>>());
	progressDialog = QPointer<ProgressDialog>(new ProgressDialog());

	QObject::connect(futureWatcher, &QFutureWatcher<QVector<ModelData>>::finished, [&]() {
		progressDialog->setRange(0, 0);
		progressDialog->setLabelText(tr("Caching assets..."));

		// Meshes
		// Note - this would be the perfect place to attach materials as well but we can't access the opengl context
		for (const auto &item : futureWatcher->result()) {
			AssetObject *model = new AssetObject(
				new AssimpObject(item.data, item.path), item.path, QFileInfo(item.path).fileName()
			);
			model->assetGuid = item.guid;
			AssetManager::addAsset(model);
		}

        for (const auto &asset : db->fetchAssetsByType(static_cast<int>(ModelTypes::File))) {
            auto assetFile = new AssetFile;
            assetFile->fileName = asset.name;
            assetFile->assetGuid = asset.guid;
            assetFile->path = IrisUtils::join(Globals::project->getProjectFolder(), asset.name);
            AssetManager::addAsset(assetFile);
        }

        for (const auto &asset : db->fetchAssetsByType(static_cast<int>(ModelTypes::Shader))) {
            QFile *templateShaderFile = new QFile(IrisUtils::join(Globals::project->getProjectFolder(), asset.name));
            templateShaderFile->open(QIODevice::ReadOnly | QIODevice::Text);
            QJsonObject shaderDefinition = QJsonDocument::fromJson(templateShaderFile->readAll()).object();
            templateShaderFile->close();
            // shaderDefinition["name"] = QFileInfo(asset.name).baseName();
            shaderDefinition.insert("guid", asset.guid);

            auto assetShader = new AssetShader;
            assetShader->assetGuid = asset.guid;
            assetShader->fileName = QFileInfo(asset.name).baseName();
            assetShader->path = IrisUtils::join(Globals::project->getProjectFolder(), asset.name);
            assetShader->setValue(QVariant::fromValue(shaderDefinition));
            AssetManager::addAsset(assetShader);
        }

        for (const auto &asset : db->fetchAssetsByType(static_cast<int>(ModelTypes::Texture))) {
            auto assetTexture = new AssetTexture;
            assetTexture->fileName = asset.name;
            assetTexture->assetGuid = asset.guid;
            assetTexture->path = IrisUtils::join(Globals::project->getProjectFolder(), asset.name);
            AssetManager::addAsset(assetTexture);
        }

		// Materials
		for (const auto &asset :
			db->fetchFilteredAssets(Globals::project->getProjectGuid(), static_cast<int>(ModelTypes::Material)))
		{
			QJsonDocument matDoc = QJsonDocument::fromBinaryData(db->fetchAssetData(asset.guid));
			QJsonObject matObject = matDoc.object();
			iris::CustomMaterialPtr material = iris::CustomMaterialPtr::create();

            QFileInfo shaderFile;

            QMapIterator<QString, QString> it(Constants::Reserved::BuiltinShaders);
            while (it.hasNext()) {
                it.next();
                if (it.key() == matObject["guid"].toString()) {
                    shaderFile = QFileInfo(IrisUtils::getAbsoluteAssetPath(it.value()));
                    break;
                }
            }

            if (shaderFile.exists()) {
                material->generate(shaderFile.absoluteFilePath());
            }
            else {
                for (auto asset : AssetManager::getAssets()) {
                    if (asset->type == ModelTypes::Shader) {
                        if (asset->assetGuid == matObject["guid"].toString()) {
                            auto def = asset->getValue().toJsonObject();
                            auto vertexShader = def["vertex_shader"].toString();
                            auto fragmentShader = def["fragment_shader"].toString();
                            for (auto asset : AssetManager::getAssets()) {
                                if (asset->type == ModelTypes::File) {
                                    if (vertexShader == asset->assetGuid) vertexShader = asset->path;
                                    if (fragmentShader == asset->assetGuid) fragmentShader = asset->path;
                                }
                            }
                            def["vertex_shader"] = vertexShader;
                            def["fragment_shader"] = fragmentShader;
                            material->generate(def);
                        }
                    }
                }
            }

			for (const auto &prop : material->properties) {
				if (prop->type == iris::PropertyType::Color) {
					QColor col;
					col.setNamedColor(matObject.value(prop->name).toString());
					material->setValue(prop->name, col);
				}
				else if (prop->type == iris::PropertyType::Texture) {
					QString materialName = db->fetchAsset(matObject.value(prop->name).toString()).name;
					QString textureStr = IrisUtils::join(Globals::project->getProjectFolder(), materialName);
					material->setValue(prop->name, !materialName.isEmpty() ? textureStr : QString());

                    qDebug() << "tex " << textureStr;
				}
				else {
					material->setValue(prop->name, QVariant::fromValue(matObject.value(prop->name)));
				}
			}

			auto assetMat = new AssetMaterial;
			assetMat->assetGuid = asset.guid;
			assetMat->setValue(QVariant::fromValue(material));
			AssetManager::addAsset(assetMat);
		}


		progressDialog->setLabelText(tr("Opening scene..."));
		emit fileToOpen(openInPlayMode);
		progressDialog->close();
	});


	QObject::connect(futureWatcher, &QFutureWatcher<QVector<ModelData>>::progressRangeChanged,
		progressDialog.data(), &ProgressDialog::setRange);
	QObject::connect(futureWatcher, &QFutureWatcher<QVector<ModelData>>::progressValueChanged,
		progressDialog.data(), &ProgressDialog::setValue);

    dynamicGrid = new DynamicGrid(this);

    settings = SettingsManager::getDefaultManager();

    ui->tilePreview->setView(new QListView());
    ui->tilePreview->setItemDelegate(new QStyledItemDelegate(ui->tilePreview));
    ui->tilePreview->setCurrentText(settings->getValue("tileSize", "Normal").toString());

    connect(ui->tilePreview,    SIGNAL(currentTextChanged(QString)), SLOT(changePreviewSize(QString)));
    connect(ui->newProject,     SIGNAL(pressed()), SLOT(newProject()));
    connect(ui->importWorld,    SIGNAL(pressed()), SLOT(importProjectFromFile()));
    connect(ui->browseProjects, SIGNAL(pressed()), SLOT(openSampleBrowser()));

    ui->browseProjects->setCursor(Qt::PointingHandCursor);

    searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);   // timer can only fire once after started

    connect(searchTimer, &QTimer::timeout, this, [this]() {
        dynamicGrid->searchTiles(searchTerm.toLower());
    });

    connect(ui->lineEdit, &QLineEdit::textChanged, this, [this](const QString &searchTerm) {
        this->searchTerm = searchTerm;
        searchTimer->start(100);
    });

    connect(ui->tilePreview, &QComboBox::currentTextChanged, [this](const QString &changedText) {
        settings->setValue("tileSize", changedText);
    });

	connect(ui->downloadWorlds, &QPushButton::pressed, []() {
		QDesktopServices::openUrl(QUrl("http://www.jahfx.com/downloads/worlds/"));
	});

    populateDesktop();

    QGridLayout *layout = new QGridLayout();
    layout->addWidget(dynamicGrid);
    layout->setMargin(0);

    ui->pmContainer->setStyleSheet(
		"border: none;"
		"background-image: url(:/images/empty_canvas.png);"
		"background-attachment: fixed;"
		"background-position: center;"
		"background-origin: content;"
		"background-repeat: no-repeat;"
	);
    ui->pmContainer->setLayout(layout);
}

ProjectManager::~ProjectManager()
{
	delete ui;
}

void ProjectManager::openProjectFromWidget(ItemGridWidget *widget, bool playMode)
{
	if (!UiManager::isSceneOpen) {
		auto spath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + Constants::PROJECT_FOLDER;
		auto projectFolder = SettingsManager::getDefaultManager()->getValue("default_directory", spath).toString();

		Globals::project->setProjectPath(QDir(projectFolder).filePath(widget->tileData.name));
		Globals::project->setProjectGuid(widget->tileData.guid);

		this->openInPlayMode = playMode;

		loadProjectAssets();
	}
	else {
		mainWindow->switchSpace(WindowSpaces::EDITOR);
	}
}

QString importProjectName;
int on_extract_entry(const char *filename, void *arg) {
    QFileInfo fInfo(filename);
    if (fInfo.suffix() == "db") importProjectName = fInfo.baseName();
    return 0;
}

void ProjectManager::importProjectFromFile(const QString& file)
{
    QString fileName;
    if (file.isEmpty()) {
        fileName = QFileDialog::getOpenFileName(this,       "Import World",
                                                nullptr,    "Jahshaka Project (*.zip)");

        if (fileName.isEmpty() || fileName.isNull()) return;
    } else {
        fileName = file;
    }

    // get the current project working directory
    auto pFldr = IrisUtils::join(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                 Constants::PROJECT_FOLDER);
    auto defaultProjectDirectory = settings->getValue("default_directory", pFldr).toString();

    // create a temporary directory and extract our project into it
    // we need a sure way to get the project name, so we have to extract it first and check the blob
    QTemporaryDir temporaryDir;
    if (temporaryDir.isValid()) {
        zip_extract(fileName.toStdString().c_str(),
                    temporaryDir.path().toStdString().c_str(),
                    on_extract_entry,
                    Q_NULLPTR);
    }

    // now extract the project to the default projects directory with the name
    if (!importProjectName.isEmpty() || !importProjectName.isNull()) {
        auto pDir = QDir(defaultProjectDirectory).filePath(importProjectName);

        zip_extract(fileName.toStdString().c_str(), pDir.toStdString().c_str(), Q_NULLPTR, Q_NULLPTR);
        QDir dir;
        if (!dir.remove(QDir(pDir).filePath(importProjectName + ".db"))) {
            // let's try again shall we...
            remove(QDir(pDir).filePath(importProjectName + ".db").toStdString().c_str());
        }

        auto open = db->importProject(QDir(temporaryDir.path()).filePath(importProjectName));
        if (open) {
            Globals::project->setProjectPath(pDir);
            loadProjectAssets();
        }
    }

    temporaryDir.remove();
}

void ProjectManager::exportProjectFromWidget(ItemGridWidget *widget)
{
    auto spath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + Constants::PROJECT_FOLDER;
    auto projectFolder = SettingsManager::getDefaultManager()->getValue("default_directory", spath).toString();

    Globals::project->setProjectPath(QDir(projectFolder).filePath(widget->tileData.name));
    Globals::project->setProjectGuid(widget->tileData.guid);

    emit exportProject();
}

void ProjectManager::renameProjectFromWidget(ItemGridWidget *widget)
{
    auto spath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + Constants::PROJECT_FOLDER;
    auto projectFolder = SettingsManager::getDefaultManager()->getValue("default_directory", spath).toString();

    QDir dir;
    auto dirRename = dir.rename(QDir(projectFolder).filePath(widget->tileData.name),
                                QDir(projectFolder).filePath(widget->labelText));
    if (dirRename) {
        widget->updateLabel(widget->labelText);
        Globals::project->setProjectGuid(widget->tileData.guid);
        db->renameProject(widget->labelText);
    }
    else {
        QMessageBox::warning(this,
                             "Rename failed",
                             "Failed to rename project, please try again or rename manually",
                             QMessageBox::Ok);
    }
}

void ProjectManager::closeProjectFromWidget(ItemGridWidget *widget)
{
    Q_UNUSED(widget);
    emit closeProject();
}

void ProjectManager::deleteProjectFromWidget(ItemGridWidget *widget)
{
    auto spath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + Constants::PROJECT_FOLDER;
    auto projectFolder = SettingsManager::getDefaultManager()->getValue("default_directory", spath).toString();

    auto option = QMessageBox::question(this,
                                        "Deleting Project",
                                        "Are you sure you want to delete this project?",
                                        QMessageBox::Yes | QMessageBox::Cancel);

    if (option == QMessageBox::Yes) {
        QDir dirToRemove(QDir(projectFolder).filePath(widget->tileData.name));
        if (dirToRemove.removeRecursively()) {
            dynamicGrid->deleteTile(widget);
            Globals::project->setProjectGuid(widget->tileData.guid);
            db->deleteProject();

			// Delete folder and contents
			for (const auto &files : db->deleteFolderAndDependencies(Globals::project->getProjectGuid())) {
				auto file = QFileInfo(QDir(Globals::project->getProjectFolder()).filePath(files));
				if (file.isFile() && file.exists()) QFile(file.absoluteFilePath()).remove();
			}

			// Delete asset and dependencies
			for (const auto &files : db->deleteAssetAndDependencies(Globals::project->getProjectGuid())) {
				auto file = QFileInfo(QDir(Globals::project->getProjectFolder()).filePath(files));
				if (file.isFile() && file.exists()) QFile(file.absoluteFilePath()).remove();
			}

            checkForEmptyState();
        } else {
            QMessageBox::warning(this,
                                 "Delete Failed!",
                                 "Failed to remove entire project folder, please try again!",
                                 QMessageBox::Ok);
        }
    }
}

void ProjectManager::searchProjects()
{
    dynamicGrid->searchTiles(ui->lineEdit->text());
}

void ProjectManager::populateDesktop(bool reset)
{
    if (reset) dynamicGrid->resetView();

    int i = 0;
    foreach (const ProjectTileData &record, db->fetchProjects()) {
        dynamicGrid->addToGridView(record, i);
        i++;
    }

    checkForEmptyState();
}

bool ProjectManager::checkForEmptyState()
{
    if (dynamicGrid->containsTiles()) {
        ui->stackedWidget->setCurrentIndex(0);
        return false;
    }

    ui->stackedWidget->setCurrentIndex(1);
    return true;
}

void ProjectManager::cleanupOnClose()
{
    AssetManager::getAssets().clear();
}

void ProjectManager::openSampleProject(QListWidgetItem *item)
{
    sampleDialog.close();
    importProjectFromFile(item->data(Qt::UserRole).toString());
}

void ProjectManager::newProject()
{
    NewProjectDialog dialog;

    dialog.exec();

    auto projectName = dialog.getProjectInfo().projectName;
    auto projectPath = dialog.getProjectInfo().projectPath;

    if (!projectName.isEmpty() || !projectName.isNull()) {
        auto fullProjectPath = QDir(projectPath).filePath(projectName);

        Globals::project->setProjectPath(fullProjectPath);

        // make a dir and the default subfolders
        QDir projectDir(fullProjectPath);
        if (!projectDir.exists()) projectDir.mkpath(".");

		QJsonObject assetProperty;

		// Insert an empty scene to get access to the project guid... 
		db->createProject(projectName);

		//for (auto folder : Constants::PROJECT_DIRS) {
		//	QDir dir(QDir(fullProjectPath).filePath(folder));
		//	dir.mkpath(".");
		//}
		
        emit fileToCreate(projectName, fullProjectPath);

        this->hide();
    }
}

void ProjectManager::changePreviewSize(QString scale)
{
    dynamicGrid->scaleTile(scale);
}

ModelData ProjectManager::loadAiSceneFromModel(const QPair<QString, QString> asset)
{
	//QFile file(asset.first);
	//file.open(QFile::ReadOnly);
	//auto data = file.readAll();

	Assimp::Importer *importer = new Assimp::Importer;
	 //const aiScene *scene = importer->ReadFile(asset.first.toStdString().c_str(), aiProcessPreset_TargetRealtime_Fast);
	//const aiScene *scene = sceneSource->importer.ReadFileFromMemory((void*)data.data(),
	//																data.length(),
	//																aiProcessPreset_TargetRealtime_Fast);
	ModelData d = { asset.first, asset.second, importer->ReadFile(asset.first.toStdString().c_str(), aiProcessPreset_TargetRealtime_Fast) };
	return d;
}

void ProjectManager::finalizeProjectAssetLoad()
{
	
}

void ProjectManager::finishedFutureWatcher()
{
    emit fileToOpen(settings->getValue("open_in_player", QVariant::fromValue(false)).toBool());
    progressDialog->close();
}

void ProjectManager::openSampleBrowser()
{
    sampleDialog.setFixedSize(Constants::TILE_SIZE * 1.66);
    sampleDialog.setWindowFlags(sampleDialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    sampleDialog.setWindowTitle("Sample Worlds");

    QGridLayout *layout = new QGridLayout();
    QListWidget *sampleList = new QListWidget();
    sampleList->setObjectName("sampleList");
    sampleList->setStyleSheet("#sampleList { background-color: #1e1e1e; padding: 0 8px; border: none; color: #EEE; } " \
                              "QListWidgetItem { padding: 12px; } "\
                              "QListView::item:selected { "\
                              "    border: 1px solid #3498db; "\
                               " background: #3498db; "\
                               "  color: #CECECE; "\
                              "} "\
                              "*, QLabel { color: white; } "\
                              "QToolTip { padding: 2px; border: 0; background: black; opacity: 200; }");
    sampleList->setViewMode(QListWidget::IconMode);
    sampleList->setSizeAdjustPolicy(QListWidget::AdjustToContents);
    sampleList->setSpacing(4);
    sampleList->setResizeMode(QListWidget::Adjust);
    sampleList->setMovement(QListView::Static);
    sampleList->setIconSize(Constants::TILE_SIZE * 0.5);
    sampleList->setSelectionMode(QAbstractItemView::SingleSelection);

    QMap<QString, QString> samples;
    samples.insert("preview/matcaps.png",   "Matcaps");
    samples.insert("preview/particles.png", "Particles");
    samples.insert("preview/skeletal.png",  "Skeletal Animation");
    samples.insert("preview/world.png",     "World Background");

    QDir dir(IrisUtils::getAbsoluteAssetPath(Constants::SAMPLES_FOLDER));

    QMap<QString, QString>::const_iterator it;
    for (it = samples.begin(); it != samples.end(); ++it){
        auto item = new QListWidgetItem();
        item->setData(Qt::DisplayRole, it.value());
        item->setData(Qt::UserRole, QDir(dir.absolutePath()).filePath(it.value()) + ".zip");
        item->setIcon(QIcon(QDir(dir.absolutePath()).filePath(it.key())));
        sampleList->addItem(item);
    }

    connect(sampleList, SIGNAL(itemDoubleClicked(QListWidgetItem*)), SLOT(openSampleProject(QListWidgetItem*)));

    auto instructions = new QLabel("Double click on a sample world to import it in the editor");
    instructions->setObjectName("instructions");
    instructions->setStyleSheet("#instructions { border: none; background: #1e1e1e; color: white; " \
                                "padding: 10px; font-size: 12px }");
    layout->addWidget(instructions);
    layout->addWidget(sampleList);
    layout->setMargin(0);
    layout->setSpacing(0);

    sampleDialog.setLayout(layout);
    sampleDialog.exec();
}

void ProjectManager::loadProjectAssets()
{
	// This shouldn't be needed but just in case a scene doesn't get cleaned up due 
	// to some future change this will prevent any subtle bugs regarding invalid data
	AssetManager::clearAssetList();

    // The whole point of the function is to concurrently load models when opening a project
    // As the project scope expands and projects get larger, it will be expanded for more (large) assets
    QVector<AssetList> assetsToLoad;

	progressDialog->setLabelText(tr("Collecting assets..."));

	// TODO - if we are only loading a couple assets, just do it sequentially
	for (const auto &asset : db->fetchFilteredAssets(Globals::project->getProjectGuid(), static_cast<int>(ModelTypes::Mesh))) {
		assetsToLoad.append(
			AssetList(QDir(Globals::project->getProjectFolder()).filePath(asset.name),
			db->fetchMeshObject(asset.guid, static_cast<int>(ModelTypes::Object), static_cast<int>(ModelTypes::Mesh)))
		);
	}

    progressDialog->setLabelText(tr("Loading assets..."));

    AssetWidgetConcurrentWrapper aiSceneFromModelMapper(this);
    auto aiSceneFromModelReducer = [](QVector<ModelData> &accum, const ModelData &interm) {
		accum.append(interm);
	};
    auto future = QtConcurrent::mappedReduced<QVector<ModelData>>(assetsToLoad.constBegin(),
																  assetsToLoad.constEnd(),
																  aiSceneFromModelMapper,
																  aiSceneFromModelReducer,
																  QtConcurrent::OrderedReduce);
    futureWatcher->setFuture(future);
    progressDialog->exec();
    futureWatcher->waitForFinished();
}

void ProjectManager::updateTile(const QString &id, const QByteArray & arr)
{
	dynamicGrid->updateTile(id, arr);
}
