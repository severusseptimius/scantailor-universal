/*
    Scan Tailor - Interactive post-processing tool for scanned pages.
    Copyright (C) 2020 Alexander Trufanov <trufanovan@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Filter.h"
#include "FilterUiInterface.h"
#include "OptionsWidget.h"
#include "Settings.h"
#include "OutputParams.h"
#include "Task.h"
#include "CacheDrivenTask.h"
#include "PageId.h"
#include "ImageId.h"
#include "ProjectReader.h"
#include "ProjectWriter.h"
#include "XmlMarshaller.h"
#include "XmlUnmarshaller.h"
#ifndef Q_MOC_RUN
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#endif
#include <QString>
#include <QObject>
#include <QCoreApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QTabBar>
#include <QMessageBox>
#include <QMenu>
#include <QFileDialog>
#include <QProcess>
#include <QDebug>

#include <iostream>
#include "CommandLine.h"
#include "StatusBarProvider.h"
#include "OrderByFileSize.h"
#include "OrderByDjbz.h"
#include "OrderByLayers.h"

#include "StageSequence.h"
#include "MetadataEditorDialog.h"
#include "ManageDisplayPreferencesDialog.h"
#include "ContentsManagerDialog.h"

#include <libdjvu/ddjvuapi.h>

// required by resource system for static libs
// must be declared in default namespace
inline void initStaticLibResources() { Q_INIT_RESOURCE(qdjvuwidget);
                                       Q_INIT_RESOURCE(dialogs);}

namespace publish
{

Filter::Filter(StageSequence* stages, IntrusivePtr<ProjectPages> const& pages,
               PageSelectionAccessor const& page_selection_accessor)
    : m_ptrStages(stages),
      m_outputFileNameGenerator(nullptr),
      m_ptrPages(pages),
      m_ptrSettings(new Settings),
      m_DjVuContext("scan_tailor_universal"),
      m_selectedPageOrder(0), m_suppressDjVuDisplay(true)
{
    initStaticLibResources();

    connect(m_ptrSettings.get(), &Settings::bundledDocReady, this, &Filter::enableBundledDjVuButton);
    connect(m_ptrSettings.get(), &Settings::bundledDjVuFilenameChanged, this, &Filter::setBundledDjVuDoc);

    if (CommandLine::get().isGui()) {
        setupImageViewer();
        m_ptrOptionsWidget.reset(
            new OptionsWidget(this, page_selection_accessor)
        );
    }


    typedef PageOrderOption::ProviderPtr ProviderPtr;
    ProviderPtr const default_order;
    ProviderPtr const order_by_filesize(new OrderByFileSize(m_ptrSettings));
    ProviderPtr const order_by_djbz(new OrderByDjbz(m_ptrSettings));
    ProviderPtr const order_by_layers(new OrderByLayers(*m_ptrStages->outputFilter()->exportSuggestions()));
    m_pageOrderOptions.push_back(PageOrderOption(tr("Natural order"), default_order));
    m_pageOrderOptions.push_back(PageOrderOption(tr("Processed then unprocessed"), ProviderPtr(new OrderByReadiness())));
    m_pageOrderOptions.push_back(PageOrderOption(tr("Order by file size"), order_by_filesize,
                                                 tr("Orders the pages by the DjVu page file size")));
    m_pageOrderOptions.push_back(PageOrderOption(tr("Order by shared dictionary"), order_by_djbz,
                                                 tr("Orders the pages by the shared dictionary id")));
    m_pageOrderOptions.push_back(PageOrderOption(tr("Order by layers"), order_by_layers,
                                                 tr("Orders the pages by type of contained layers")));
}

Filter::~Filter()
{
}

void Filter::setupImageViewer()
{
    QWidget* wgt = new QWidget();
    m_ptrImageViewer.reset(wgt);
    QVBoxLayout* lt = new QVBoxLayout(wgt);
    lt->setSpacing(0);

    QTabBar* tab = new QTabBar();
    tab->setObjectName("tab");
    tab->addTab(tr("Main"));
    tab->addTab(tr("Foreground"));
    tab->addTab(tr("Background"));
    tab->addTab(tr("B&W Mask"));
    tab->addTab(tr("Text"));

    lt->addWidget(tab, 0, Qt::AlignTop);
    m_ptrDjVuWidget.reset( new QDjVuWidget() );
    lt->addWidget(m_ptrDjVuWidget.get(), 100);

    connect(tab, &QTabBar::currentChanged, this, &Filter::tabChanged );
}

void
Filter::suppressDjVuDisplay(const PageId& page_id, bool val)
{
    bool update_display = !val && m_suppressDjVuDisplay;
    m_suppressDjVuDisplay = val;

    if (update_display) {
        updateDjVuDocument(page_id);
    }
}

void
Filter::updateDjVuDocument(const PageId& page_id)
{
    qDebug() << "updateDjVuDocument " << page_id.imageId().filePath();
    std::unique_ptr<Params> params = m_ptrSettings->getPageParams(page_id);
    const QString djvu_filename = params->djvuFilename();

    StatusBarProvider::setFileSize(params->djvuSize());

    if (!m_suppressDjVuDisplay) {
        if (QFileInfo::exists(djvu_filename)) {

            ddjvu_cache_clear(m_DjVuContext);
            QDjVuDocument* doc = new QDjVuDocument(true);
            doc->setFileName(&m_DjVuContext, djvu_filename, false);

            if (!doc->isValid()) {
                delete doc;
                QMessageBox::critical(qApp->activeWindow(), tr("Cannot open file '%1'.").arg(djvu_filename), tr("Opening DjVu file"));
            } else {
                // technically takes doc ownership as it was created with autoDel==true
                connect(doc, &QDjVuDocument::error, [](QString err, QString fname, int line_no) {
                    qDebug() << err << fname << line_no;
                });
                m_ptrDjVuWidget->setDocument(doc);
                m_ptrDjVuWidget->setAlternativeImage(params->sourceImagesInfo().source_filename());
                QColor clr(Qt::red);
                clr.setAlpha(60);
                m_ptrDjVuWidget->addHighlight(0, 1, 1, 1000, 1000, clr);
                m_ptrImageViewer->show();

                QTabBar* tab = m_ptrImageViewer->findChild<QTabBar*>("tab");
                assert(tab);
                emit tab->currentChanged(tab->currentIndex());
            }
        } else {
            m_ptrImageViewer->hide();
        }
    }

    emit m_ptrOptionsWidget->invalidateThumbnail(page_id);
}

QString
Filter::getName() const
{
    return QCoreApplication::translate(
               "publishing::Filter", "Make a book"
           );
}

PageView
Filter::getView() const
{
    return PAGE_VIEW;
}

int
Filter::selectedPageOrder() const
{
    return m_selectedPageOrder;
}

void
Filter::selectPageOrder(int option)
{
    assert((unsigned)option < m_pageOrderOptions.size());
    m_selectedPageOrder = option;
}

std::vector<PageOrderOption>
Filter::pageOrderOptions() const
{
    return m_pageOrderOptions;
}

void
Filter::performRelinking(AbstractRelinker const& relinker)
{
    m_ptrSettings->performRelinking(relinker);
}

void
Filter::preUpdateUI(FilterUiInterface* ui, PageId const& page_id)
{
    if (m_ptrOptionsWidget.get()) {
        m_ptrOptionsWidget->preUpdateUI(page_id);
        ui->setOptionsWidget(m_ptrOptionsWidget.get(), ui->KEEP_OWNERSHIP);
    }
}

QDomElement
Filter::saveSettings(
    ProjectWriter const& writer, QDomDocument& doc) const
{
    QDomElement filter_el(doc.createElement("publishing"));

    filter_el.setAttribute("bundled_name", m_ptrSettings->bundledDocFilename());
    filter_el.setAttribute("bundled_size", m_ptrSettings->bundledDocFilesize());
    filter_el.setAttribute("bundled_modified", m_ptrSettings->bundledDocModified().toString("dd.MM.yyyy hh:mm:ss.zzz"));

    writer.enumPages(
        boost::lambda::bind(
            &Filter::writePageSettings,
            this, boost::ref(doc), boost::lambda::var(filter_el), boost::lambda::_1, boost::lambda::_2
        )
    );

    if (m_ptrSettings->djbzDispatcher()) {
        filter_el.appendChild(m_ptrSettings->djbzDispatcher()->toXml(doc, "djbz_dispatcher"));
    }

    if (m_ptrSettings->djVuBookmarkDispatcher()) {
        filter_el.appendChild(m_ptrSettings->djVuBookmarkDispatcher()->toXml(doc, "bookmarks"));
    }

    const QMap<QString, QString>& metadata = m_ptrSettings->metadataRef();
    QDomElement metadata_el(doc.createElement("metadata"));
    for (QMap<QString, QString>::const_iterator it = metadata.cbegin();
         it != metadata.cend(); it++) {
        if (!it.key().isEmpty()) {
            metadata_el.setAttribute(it.key(), *it);
        }
    }
    filter_el.appendChild(metadata_el);

    if (!m_ptrSettings->displayPreferences().isEmpty()) {
        filter_el.appendChild(m_ptrSettings->displayPreferences().toXml(doc, "display_prefs"));
    }

    if (!m_ptrSettings->contents().isEmpty()) {
        QDomElement contents_el(doc.createElement("contents"));
        QDomText text = doc.createTextNode(m_ptrSettings->contents().join('\n'));
        contents_el.appendChild(text);
        filter_el.appendChild(contents_el);
    }

    return filter_el;
}

void
Filter::loadSettings(ProjectReader const& reader, QDomElement const& filters_el)
{
    m_ptrSettings->clear();

    const QDomElement filter_el(filters_el.namedItem("publishing").toElement());

    const QString bundled_name = filter_el.attribute("bundled_name", "");
    m_ptrSettings->setBundledDocFilename(bundled_name);
    if (!bundled_name.isEmpty()) {
        bool reseted = false;
        if (filter_el.hasAttribute("bundled_size")) {
            uint size = filter_el.attribute("bundled_size").toUInt();
            if (m_ptrSettings->bundledDocFilesize() != size) {
                m_ptrSettings->resetBundledDoc();
                reseted = true;
            }
        }

        if (!reseted && filter_el.hasAttribute("bundled_modified")) {
            QDateTime modif = QDateTime::fromString(filter_el.attribute("bundled_modified"));
            if (m_ptrSettings->bundledDocModified() != modif) {
                m_ptrSettings->resetBundledDoc();
            }
        }
    }



    m_ptrSettings->setDjbzDispatcher(new DjbzDispatcher(filter_el.namedItem("djbz_dispatcher").toElement()) );
    m_ptrSettings->setDjVuBookmarkDispatcher( new DjVuBookmarkDispatcher(filter_el.namedItem("bookmarks").toElement()) );

    const QDomNodeList metadata_els = filter_el.elementsByTagName("metadata");
    if (!metadata_els.isEmpty()) {
        assert(metadata_els.size() == 1);
        const QDomElement metadata_el = metadata_els.at(0).toElement();
        const QDomNamedNodeMap vals = metadata_el.attributes();
        QMap<QString, QString> metadata;
        for ( int i = 0; i < vals.count(); i++) {
            const QDomNode dn = vals.item(i);
            metadata[dn.nodeName()] = dn.nodeValue();
        }
        m_ptrSettings->setMetadata(metadata);
    } else {
        m_ptrSettings->setMetadata(MetadataEditorDialog::getDefaultMetadata());
    }

    const QDomNodeList display_pref_els = filter_el.elementsByTagName("display_prefs");
    if (!display_pref_els.isEmpty()) {
        assert(display_pref_els.size() == 1);
        const QDomElement display_pref_el = display_pref_els.at(0).toElement();
        DisplayPreferences prefs(display_pref_el);
        m_ptrSettings->setDisplayPreferences(prefs);
    }


    const QDomNodeList contents_els = filter_el.elementsByTagName("contents");
    if (!contents_els.isEmpty()) {
        assert(contents_els.size() == 1);
        const QDomElement contents_el = metadata_els.at(0).toElement();
        m_ptrSettings->setContents(contents_el.text().split('\n', QString::SkipEmptyParts));
    }

    QString const page_tag_name("page");
    QDomNode node(filter_el.firstChild());
    for (; !node.isNull(); node = node.nextSibling()) {
        if (!node.isElement()) {
            continue;
        }
        if (node.nodeName() != page_tag_name) {
            continue;
        }
        QDomElement const el(node.toElement());

        bool ok = true;
        int const id = el.attribute("id").toInt(&ok);
        if (!ok) {
            continue;
        }

        PageId const page_id(reader.pageId(id));
        if (page_id.isNull()) {
            continue;
        }

        QDomElement const params_el(el.namedItem("params").toElement());
        if (params_el.isNull()) {
            continue;
        }

        Params const params(params_el);
        m_ptrSettings->setPageParams(page_id, params);
        m_ptrSettings->djbzDispatcher()->addToDictionary(page_id, params.djbzId(), true);
    }
}

void
Filter::invalidateSetting(PageId const& page_id)
{
    std::unique_ptr<Params> const params(m_ptrSettings->getPageParams(page_id));
    if (params.get()) {
        Params p(*params.get());
        p.setForceReprocess(Params::RegenerateAll);
        m_ptrSettings->setPageParams(page_id, p);
    }
}


void
Filter::ensureAllPagesHaveDjbz(DjbzDispatcher& dispatcher)
{
    dispatcher.autosetToDictionaries(m_ptrPages->toPageSequence(PAGE_VIEW),
                                   m_ptrStages->outputFilter()->exportSuggestions(),
                                   m_ptrSettings);
}
void
Filter::ensureAllPagesHaveDjbz()
{
    ensureAllPagesHaveDjbz(*m_ptrSettings->djbzDispatcher());
}

void
Filter::reassignAllPagesExceptLocked(DjbzDispatcher& dispatcher)
{
    dispatcher.resetNonLockedDictionaries();

    PageSequence pages = m_ptrPages->toPageSequence(PAGE_VIEW);
    for (const PageInfo& p: pages) {
        QString dict_id = dispatcher.dictionaryIdByPage(p.id());
        std::unique_ptr<Params> params_ptr(m_ptrSettings->getPageParams(p.id()));
        if (params_ptr->djbzId() != dict_id) {
            params_ptr->setDjbzId(dict_id);
            m_ptrSettings->setPageParams(p.id(), *params_ptr);
        }
    }

    ensureAllPagesHaveDjbz(dispatcher);
}

IntrusivePtr<Task>
Filter::createTask(
    PageId const& page_id,
    IntrusivePtr<ThumbnailPixmapCache> const& thumbnail_cache,
    OutputFileNameGenerator const& out_file_name_gen,
    bool const batch_processing)
{
    IntrusivePtr<Task> task(
                   new Task(
                       page_id, IntrusivePtr<Filter>(this),
                       m_ptrSettings, thumbnail_cache, out_file_name_gen, batch_processing
                   )
               );
    // must be done via slots as both objects are in different threads
//    connect(task.get(), &Task::displayDjVu, this, &Filter::updateDjVuDocument);
    connect(task.get(), &Task::setProgressPanelVisible, this, &Filter::setProgressPanelVisible);
    connect(task.get(), &Task::displayProgressInfo, this, &Filter::displayProgressInfo);
    connect(task.get(), &Task::generateBundledDocument, this, &Filter::makeBundledDjVu);

    m_ptrDjVuWidget->setDocument(new QDjVuDocument(true)/*nullptr*/);
    m_ptrDjVuWidget->update();
    return task;
}

IntrusivePtr<CacheDrivenTask>
Filter::createCacheDrivenTask(const OutputFileNameGenerator &out_file_name_gen)
{
    return IntrusivePtr<CacheDrivenTask>(
               new CacheDrivenTask(m_ptrSettings, out_file_name_gen)
           );
}

void
Filter::writePageSettings(
    QDomDocument& doc, QDomElement& filter_el,
    PageId const& page_id, int const numeric_id) const
{
    std::unique_ptr<Params> const params(m_ptrSettings->getPageParams(page_id));
    if (!params.get()) {
        return;
    }

    QDomElement page_el(doc.createElement("page"));
    page_el.setAttribute("id", numeric_id);
    page_el.appendChild(params->toXml(doc, "params"));

    filter_el.appendChild(page_el);
}

IntrusivePtr<CompositeCacheDrivenTask>
Filter::createCompositeCacheDrivenTask()
{
    if (m_outputFileNameGenerator) {
        return m_ptrStages->createCompositeCacheDrivenTask(*m_outputFileNameGenerator, m_ptrStages->publishFilterIdx());
    } else {
        return IntrusivePtr<CompositeCacheDrivenTask>();
    }
}

void
Filter::displayDbjzManagerDlg()
{
    if (m_ptrOptionsWidget) {
        m_ptrOptionsWidget->on_lblDjbzId_linkActivated(QString());
    }
}

QVector<PageInfo>
Filter::filterBatchPages(const QVector<PageInfo>& pages) const
{
    QVector<PageInfo> res;
    QSet<QString> known_djbz;
    for (const PageInfo& page: pages) {
        std::unique_ptr<Params> params = m_ptrSettings->getPageParams(page.id());
        if (params) {
            QString dict_id = params->djbzId();
            if (!known_djbz.contains(dict_id) ||
                    m_ptrSettings->djbzDispatcher()->dictionary(dict_id)->isDummy()) {
                res += page;
                known_djbz += dict_id;
            }
        } else {
            res += page;
        }
    }
    return res;
}

static bool __makingBundledDjVu_in_progress = false;

void
Filter::makeBundledDjVu()
{
    if (__makingBundledDjVu_in_progress) {
        return;
    }

    __makingBundledDjVu_in_progress = true;

    const std::vector<PageId> all_pages_ordered =
            m_ptrPages->toPageSequence(PAGE_VIEW).asPageIdVector();

    QString bundled_DjVu_fname = m_ptrSettings->bundledDocFilename();

    if (bundled_DjVu_fname.isEmpty()) {

        bundled_DjVu_fname = m_bundledDjVuSuggestion;

        QFileDialog dlg(qApp->activeWindow(),
                        tr("Save bundled DjVu document"),
                        bundled_DjVu_fname,
                        tr("DjVu documents (*.djvu *.djv)"));
        dlg.setAcceptMode(QFileDialog::AcceptSave);
        if (dlg.exec() == QDialog::Accepted) {
            const QStringList sl = dlg.selectedFiles();
            bundled_DjVu_fname = sl.first();
            m_ptrSettings->setBundledDocFilename(bundled_DjVu_fname);
        } else {
            bundled_DjVu_fname.clear();
        }
    }

    if (!bundled_DjVu_fname.isEmpty()) {
        QStringList args;
        args << "-c";
        args << bundled_DjVu_fname;
        for (const PageId& p: all_pages_ordered) {
            std::unique_ptr<Params> params_ptr = m_ptrSettings->getPageParams(p);
            args << params_ptr->djvuFilename();
        }

        QProcess proc;
        proc.setProcessChannelMode(QProcess::QProcess::ForwardedChannels);
        proc.start(GlobalStaticSettings::m_djvu_bin_djvm, args);
        proc.waitForFinished(-1);
    }

    /************************************
     * Postprocess bundled DjVu
     ************************************/
    if (!bundled_DjVu_fname.isEmpty()) {
        QString djvused_cmd;

        // set metadata

        const QString meta_fname = m_outputFileNameGenerator->outDir() + "/" +
                GlobalStaticSettings::m_djvu_pages_subfolder + "/" + "document.meta";
        const QMap<QString, QString>& metadata = m_ptrSettings->metadataRef();
        if (!metadata.isEmpty()) {
            QFile f(meta_fname);
            f.open(QIODevice::WriteOnly | QFile::Truncate);
            QTextStream ts;
            ts.setDevice(&f);

            for (QMap<QString, QString>::const_iterator it = metadata.constBegin();
                 it != metadata.constEnd(); ++it) {
                ts << it.key() + "        " + it.value() << "\n";
            }

            f.close();

            djvused_cmd += QString("select ; set-meta \"%1\" ; ").arg(meta_fname);
        } else if (QFile::exists(meta_fname)) {
            QFile::remove(meta_fname);
        }

        // set annotations

        const QString anno_fname = m_outputFileNameGenerator->outDir() + "/" +
                GlobalStaticSettings::m_djvu_pages_subfolder + "/" + "document.anno";
        const DisplayPreferences& prefs = m_ptrSettings->displayPreferences();
        if (prefs.hasAnnotations()) {
            QFile f(anno_fname);
            f.open(QIODevice::WriteOnly | QFile::Truncate);
            QTextStream ts;
            ts.setDevice(&f);
            if (!prefs.background().isEmpty()) {
                ts << QString("(background %1)\n").arg(prefs.background().toUpper());
            }
            if (!prefs.zoom().isEmpty()) {
                ts << QString("(zoom %1)\n").arg(prefs.zoom());
            }
            if (!prefs.mode().isEmpty()) {
                ts << QString("(mode %1)\n").arg(prefs.mode());
            }
            if (!prefs.alignX().isEmpty()) {
                ts << QString("(align %1 %2)\n").arg(prefs.alignX(), prefs.alignY());
            }

            f.close();

            djvused_cmd += QString("select-shared-ant ; set-ant \"%1\" ; ").arg(anno_fname);

        } else if (QFile::exists(anno_fname)) {
            djvused_cmd += "select-shared-ant ; remove-ant ; ";
            QFile::remove(anno_fname);
        }


        djvused_cmd += "remove-thumbnails ; ";
        if (prefs.thumbSize() >= 32 && prefs.thumbSize() <= 256) {
            djvused_cmd += QString("set-thumbnails \"%1\" ; ").arg(prefs.thumbSize());
        }

        // set outlines (contents)

        const QString contents_fname = m_outputFileNameGenerator->outDir() + "/" +
                GlobalStaticSettings::m_djvu_pages_subfolder + "/" + "document.contents";
        if (!m_ptrSettings->djVuBookmarkDispatcher()->isEmpty()) {
            QStringList page_uids;

            if (!GlobalStaticSettings::m_djvu_contents_as_id) {
                const PageSequence pages = m_ptrPages->toPageSequence(PAGE_VIEW);
                for (const PageInfo& p: pages) {
                    page_uids += QString("#%1.djvu").arg(
                                QFileInfo(m_outputFileNameGenerator->fileNameFor(p.id())).completeBaseName());
                }
            }
            const QStringList outlines = m_ptrSettings->djVuBookmarkDispatcher()->toDjVuSed(page_uids);

            QFile f(contents_fname);
            f.open(QIODevice::WriteOnly | QFile::Truncate);
            QTextStream ts;
            ts.setDevice(&f);
            for (const QString& s: outlines) {
                ts << s;
            }
            f.close();

            djvused_cmd += QString("select ; set-outline \"%1\" ; ").arg(contents_fname);
        } else if (QFile::exists(contents_fname)) {
            djvused_cmd += "select ; remove-outline ; ";
            QFile::remove(contents_fname);
        }


        // set per page settings

        int page_no = 0;

        for (const PageId& p: all_pages_ordered) {
            page_no++;
            std::unique_ptr<Params> params_ptr = m_ptrSettings->getPageParams(p);
            if (!params_ptr->pageTitle().isEmpty()) {
                djvused_cmd += QString("select %1; set-page-title \"%2\"; ")
                        .arg(page_no)
                        .arg(params_ptr->pageTitle());
            }
            if (params_ptr->pageRotation()) {
                djvused_cmd += QString("select %1; set-rotation \"%2\"; ")
                        .arg(page_no)
                        .arg(params_ptr->pageRotation());
            }
        }



        if (!djvused_cmd.isEmpty()) {
            QStringList args;
            args << bundled_DjVu_fname << "-e" << djvused_cmd << "-s";
            QProcess proc;
            proc.setProcessChannelMode(QProcess::QProcess::ForwardedChannels);
            proc.start(GlobalStaticSettings::m_djvu_bin_djvused, args);
            proc.waitForFinished(-1);
        }

        m_ptrSettings->updateBundledDoc();
        emit enableBundledDjVuButton(true);
    }
    __makingBundledDjVu_in_progress = false;
}

void
Filter::setBundledFilename(const QString& fname)
{
    m_ptrSettings->setBundledDocFilename(fname);
}

const QString&
Filter::bundledFilename() const
{
    return m_ptrSettings->bundledDocFilename();
}

bool
Filter::checkReadyToBundle() const
{
    const PageSequence page_seq = m_ptrPages->toPageSequence(PAGE_VIEW);
    const std::vector<PageId> all_pages_ordered = page_seq.asPageIdVector();
    return m_ptrSettings->checkPagesReady(all_pages_ordered);
}

void
Filter::displayMetadataEditor()
{
    MetadataEditorDialog dialog(m_ptrSettings->metadataRef(), qApp->activeWindow());
    if (dialog.exec() == QDialog::Accepted) {
        const auto meta = dialog.getMetadata();
        if (meta != m_ptrSettings->metadataRef()) {
            m_ptrSettings->setMetadata(dialog.getMetadata());
            m_ptrSettings->resetBundledDoc();
            emit m_ptrOptionsWidget->reloadRequested();
        }
    }
}

void
Filter::displayDisplayPreferencesEditor()
{
    ManageDisplayPreferencesDialog dialog(m_ptrSettings->displayPreferences(),  qApp->activeWindow());
    if (dialog.exec() == QDialog::Accepted) {
        const DisplayPreferences& prefs = dialog.preferences();
        if (prefs != m_ptrSettings->displayPreferences()) {
            m_ptrSettings->setDisplayPreferences(prefs);
            m_ptrSettings->resetBundledDoc();
            emit m_ptrOptionsWidget->reloadRequested();
        }
    }
}

void
Filter::displayContentsManagerDlg()
{
    ContentsManagerDialog dialog(this, qApp->activeWindow());
    if (dialog.exec() == QDialog::Accepted) {
        m_ptrSettings->setDjVuBookmarkDispatcher(new DjVuBookmarkDispatcher(dialog.bookmarks()) );
        m_ptrSettings->resetBundledDoc();
        emit m_ptrOptionsWidget->reloadRequested();
    }
}

QString
Filter::djvuFilenameForPage(const PageId& page)
{
    if (m_ptrStages) {
        std::unique_ptr<Params> params = m_ptrSettings->getPageParams(page);
        if (params) {
            return params->djvuFilename();
        }
    }
    return QString();
}


} // namespace publish

