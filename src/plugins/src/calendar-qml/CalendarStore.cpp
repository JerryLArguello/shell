#include "CalendarStore.h"
#include "Atomic.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QTimer>
#include <QtCore/QTextStream>
#include <QtCore/QUuid>
#include <QtCore/QRegularExpression>

#include <libical/ical.h>

namespace {

QString homePath()        { return QDir::homePath(); }
QString khalConfigPath()  { return homePath() + QStringLiteral("/.config/khal/config"); }
QString defaultStorageRoot() { return homePath() + QStringLiteral("/.local/share/khal/calendars"); }

QString cleanIniValue(QString v) {
    v = v.trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
        v = v.mid(1, v.size() - 2);
    if (v.size() >= 2 && v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\'')))
        v = v.mid(1, v.size() - 2);
    return v;
}

QString expandTilde(QString p) {
    if (p.startsWith(QStringLiteral("~/"))) return homePath() + p.mid(1);
    return p;
}

// Recursively scan for *.ics files. Tracks visited canonical paths to
// guard against symlink loops.
void scanIcsRecursive(const QString& dir, QStringList& out, QSet<QString>& seen) {
    const QFileInfo here(dir);
    if (!here.exists()) return;
    const QString canonical = here.canonicalFilePath();
    if (canonical.isEmpty() || seen.contains(canonical)) return;
    seen.insert(canonical);

    const QDir d(dir);
    const auto entries = d.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fi : entries) {
        if (fi.isDir())
            scanIcsRecursive(fi.absoluteFilePath(), out, seen);
        else if (fi.isFile() && fi.suffix().compare(QLatin1String("ics"), Qt::CaseInsensitive) == 0)
            out << fi.absoluteFilePath();
    }
}

// Drop CATEGORIES properties from a VEVENT (call before adding new ones).
void clearCategories(icalcomponent* vevent) {
    icalproperty* p = icalcomponent_get_first_property(vevent, ICAL_CATEGORIES_PROPERTY);
    while (p) {
        icalproperty* next = icalcomponent_get_next_property(vevent, ICAL_CATEGORIES_PROPERTY);
        icalcomponent_remove_property(vevent, p);
        icalproperty_free(p);
        p = next;
    }
}

void clearRrule(icalcomponent* vevent) {
    icalproperty* p = icalcomponent_get_first_property(vevent, ICAL_RRULE_PROPERTY);
    while (p) {
        icalproperty* next = icalcomponent_get_next_property(vevent, ICAL_RRULE_PROPERTY);
        icalcomponent_remove_property(vevent, p);
        icalproperty_free(p);
        p = next;
    }
}

// Locate the master VEVENT (UID match, no RECURRENCE-ID) inside a parsed VCALENDAR.
icalcomponent* findMasterVevent(icalcomponent* root, const QString& uid) {
    for (icalcomponent* ve = icalcomponent_get_first_component(root, ICAL_VEVENT_COMPONENT);
         ve; ve = icalcomponent_get_next_component(root, ICAL_VEVENT_COMPONENT)) {
        if (icalcomponent_get_first_property(ve, ICAL_RECURRENCEID_PROPERTY)) continue;
        if (QString::fromUtf8(icalcomponent_get_uid(ve)) == uid) return ve;
    }
    return nullptr;
}

} // namespace

CalendarStore::CalendarStore(QObject* parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_debounce(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(200);
    connect(m_debounce, &QTimer::timeout, this, &CalendarStore::debouncedRescan);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &CalendarStore::handleFsEvent);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &CalendarStore::handleFsEvent);
}

CalendarStore::~CalendarStore() = default;

// ── Slots ─────────────────────────────────────────────────────────────

void CalendarStore::rescanAll() {
    m_calendars = discoverCalendars();
    m_calendarPaths.clear();
    for (const auto& c : m_calendars)
        m_calendarPaths.insert(c.name, c.path);

    m_events.clear();
    QSet<QString> seenDirs;
    for (const auto& cal : m_calendars) {
        QStringList icsFiles;
        scanIcsRecursive(cal.path, icsFiles, seenDirs);
        for (const QString& path : icsFiles) {
            auto parsed = IcalParser::parseFile(path, cal.name);
            for (auto& ev : parsed) m_events.push_back(std::move(ev));
        }
    }
    rebuildWatchPaths();
    emit storeRefreshed(calendarNamesInternal());
}

void CalendarStore::requestRange(QDate from, QDate to) {
    emit rangeReady(from, to, eventsInRange(from, to), eventsByDate(from, to));
}

void CalendarStore::requestDay(QDate date) {
    emit dayReady(date, eventsOnDate(date));
}

void CalendarStore::requestReminderWindow(int days) {
    const QDate today = QDate::currentDate();
    if (days < 1) days = 2;
    emit reminderWindowReady(eventsInRange(today, today.addDays(days)));
}

// ── Read helpers ──────────────────────────────────────────────────────

QStringList CalendarStore::calendarNamesInternal() const {
    QStringList out;
    out.reserve(int(m_calendars.size()));
    for (const auto& c : m_calendars) out << c.name;
    return out;
}

QVariantList CalendarStore::eventsInRange(const QDate& from, const QDate& to) const {
    return IcalParser::expandRange(m_events, from, to);
}

QVariantList CalendarStore::eventsOnDate(const QDate& d) const {
    QVariantList all = eventsInRange(d, d);
    QVariantList out;
    const QDateTime dayStart(d, QTime(0, 0));
    const QDateTime dayEnd(d.addDays(1), QTime(0, 0));
    for (const auto& v : all) {
        QVariantMap m = v.toMap();
        QDateTime st = m.value(QStringLiteral("start")).toDateTime();
        QDateTime en = m.value(QStringLiteral("end")).toDateTime();
        const bool allDay = m.value(QStringLiteral("allDay")).toBool();
        // All-day end is exclusive per RFC 5545. Timed events ending
        // exactly at midnight likewise don't intrude into the next day.
        const bool endsExclusiveAtMidnight = en.isValid() && en > st &&
            (allDay || en.time() == QTime(0, 0));
        const bool overlapsEnd = endsExclusiveAtMidnight ? en > dayStart : en >= dayStart;
        if (overlapsEnd && st < dayEnd) out.append(m);
    }
    return out;
}

QVariantMap CalendarStore::eventsByDate(const QDate& from, const QDate& to) const {
    QVariantMap out;
    if (!from.isValid() || !to.isValid() || from > to) return out;

    const QVariantList all = eventsInRange(from, to);
    for (const auto& v : all) {
        QVariantMap m = v.toMap();
        QDateTime st = m.value(QStringLiteral("start")).toDateTime();
        QDateTime en = m.value(QStringLiteral("end")).toDateTime();
        const bool allDay = m.value(QStringLiteral("allDay")).toBool();
        if (!st.isValid()) continue;

        QDate ds = st.date();
        QDate de = en.isValid() ? en.date() : ds;
        if (allDay && en.isValid() && en > st) {
            // All-day DTEND is exclusive per RFC 5545. Some sources emit
            // DTEND as DATE-TIME in UTC, which shifts off midnight after
            // local-zone conversion — so don't gate on en.time() == 00:00.
            de = de.addDays(-1);
        } else if (en.isValid() && en.time() == QTime(0, 0) && en > st) {
            // Timed event ending exactly at midnight — previous day
            de = de.addDays(-1);
        }
        if (de < ds) de = ds;
        if (ds < from) ds = from;
        if (de > to)   de = to;

        for (QDate d = ds; d <= de; d = d.addDays(1)) {
            const QString key = d.toString(QStringLiteral("yyyy-MM-dd"));
            QVariantList list = out.value(key).toList();
            list.append(m);
            out.insert(key, list);
        }
    }
    return out;
}

const Event* CalendarStore::findMaster(const QString& uid) const {
    for (const Event& ev : m_events)
        if (ev.uid == uid && !ev.isOverride()) return &ev;
    return nullptr;
}

QString CalendarStore::calendarPath(const QString& name) const {
    return m_calendarPaths.value(name);
}

QString CalendarStore::newIcsPath(const QString& calendarName, const QString& uid) const {
    QString dir = calendarPath(calendarName);
    if (dir.isEmpty()) return QString();
    QDir().mkpath(dir);
    QString safe = uid;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (safe.isEmpty()) safe = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return dir + QLatin1Char('/') + safe + QStringLiteral(".ics");
}

// ── Mutation slots ────────────────────────────────────────────────────

void CalendarStore::addEventCmd(QString calendarName, QVariantMap fields) {
    QString error;
    bool ok = false;

    if (!m_calendarPaths.contains(calendarName)) {
        error = QStringLiteral("Unknown calendar: ") + calendarName;
    } else {
        QString uid = fields.value(QStringLiteral("uid")).toString();
        if (uid.isEmpty()) {
            uid = QUuid::createUuid().toString(QUuid::WithoutBraces) +
                  QStringLiteral("@quickshell");
        }
        fields.insert(QStringLiteral("uid"), uid);

        const QString text = IcalParser::buildIcsString(fields);
        if (text.isEmpty()) {
            error = QStringLiteral("Failed to build iCalendar text");
        } else {
            const QString path = newIcsPath(calendarName, uid);
            if (path.isEmpty()) {
                error = QStringLiteral("Calendar path not resolvable");
            } else if (Atomic::writeFile(path, text.toUtf8(), &error)) {
                ok = true;
                markSelfMutated();
                rescanAll();
            }
        }
    }
    emit mutationDone(ok, error);
}

void CalendarStore::editEventCmd(QString uid, QVariantMap fields,
                                 QString scopeStr, QDateTime occurrenceStart) {
    QString error;
    bool ok = false;

    const Event* master = findMaster(uid);
    if (!master) {
        error = QStringLiteral("Event not found: ") + uid;
    } else {
        const EditScope scope = editScopeFromString(scopeStr);
        fields.insert(QStringLiteral("uid"), uid);

        if (scope == EditScope::ThisInstance &&
            !master->rruleRaw.isEmpty() && occurrenceStart.isValid())
        {
            ok = editSingleOverride(*master, fields, occurrenceStart, &error);
        } else {
            ok = editAllInPlace(*master, fields, &error);
        }
        if (ok) {
            markSelfMutated();
            rescanAll();
        }
    }
    emit mutationDone(ok, error);
}

void CalendarStore::deleteEventCmd(QString uid, QString scopeStr, QDateTime occurrenceStart) {
    QString error;
    bool ok = false;

    const Event* master = findMaster(uid);
    if (!master) {
        error = QStringLiteral("Event not found: ") + uid;
    } else {
        const EditScope scope = editScopeFromString(scopeStr);
        if (scope == EditScope::ThisInstance &&
            !master->rruleRaw.isEmpty() && occurrenceStart.isValid())
        {
            ok = deleteSingleOccurrence(*master, occurrenceStart, &error);
        } else {
            ok = deleteWholeSeries(*master, &error);
        }
        if (ok) {
            markSelfMutated();
            rescanAll();
        }
    }
    emit mutationDone(ok, error);
}

// ── Mutation primitives ───────────────────────────────────────────────

bool CalendarStore::editAllInPlace(const Event& master, const QVariantMap& fields,
                                   QString* errorOut)
{
    QFile in(master.sourceFile);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot read ") + master.sourceFile;
        return false;
    }
    QByteArray data = in.readAll();
    in.close();

    ical::ComponentPtr root = IcalParser::parseToComponent(data);
    if (!root) {
        if (errorOut) *errorOut = QStringLiteral("Parse error in ") + master.sourceFile;
        return false;
    }
    icalcomponent* ve = findMasterVevent(root.get(), master.uid);
    if (!ve) {
        if (errorOut) *errorOut = QStringLiteral("Master VEVENT not found: ") + master.uid;
        return false;
    }

    // Mutate only changed fields. Setters replace existing properties
    // for SUMMARY/DESCRIPTION/LOCATION/DTSTART/DTEND. CATEGORIES and
    // RRULE need explicit clear+add.
    const QString title = fields.value(QStringLiteral("title")).toString();
    icalcomponent_set_summary(ve, title.toUtf8().constData());

    const QString desc = fields.value(QStringLiteral("description")).toString();
    icalcomponent_set_description(ve, desc.toUtf8().constData());

    const QString loc = fields.value(QStringLiteral("location")).toString();
    icalcomponent_set_location(ve, loc.toUtf8().constData());

    const bool allDay = fields.value(QStringLiteral("allDay")).toBool();
    QDateTime start = fields.value(QStringLiteral("start")).toDateTime();
    QDateTime end   = fields.value(QStringLiteral("end")).toDateTime();
    if (start.isValid()) icalcomponent_set_dtstart(ve, IcalParser::qDateTimeToIcalTime(start, allDay));
    if (end.isValid())   icalcomponent_set_dtend(ve,   IcalParser::qDateTimeToIcalTime(end, allDay));

    // CATEGORIES: clear + re-add
    clearCategories(ve);
    QStringList cats = fields.value(QStringLiteral("categories")).toStringList();
    if (!cats.isEmpty()) {
        QByteArray joined = cats.join(QLatin1Char(',')).toUtf8();
        icalcomponent_add_property(ve, icalproperty_new_categories(joined.constData()));
    }

    // RRULE: only touch if recurrence keyword changed compared to master.
    // This preserves complex clauses (BYDAY, INTERVAL>1, COUNT, BYSETPOS)
    // when the user edits an unrelated field like the title.
    const Recurrence newRec = recurrenceFromString(
        fields.value(QStringLiteral("recurrence")).toString());
    if (newRec != master.recurrence) {
        clearRrule(ve);
        if (newRec != Recurrence::None) {
    struct icalrecurrencetype rule;
    icalrecurrencetype_clear(&rule);
    rule.freq = toIcalFreq(newRec);
    rule.interval = 1;
    QDate until = fields.value(QStringLiteral("recurrenceUntil")).toDate();
    if (until.isValid()) {
        icaltimetype u = icaltime_null_date();
        u.year = until.year(); u.month = until.month(); u.day = until.day();
        u.is_date = 1;
        rule.until = u;
    }
    icalcomponent_add_property(ve, icalproperty_new_rrule(rule));
}
    } else if (newRec != Recurrence::None) {
        // Recurrence keyword unchanged, but UNTIL might have changed.
        // Update UNTIL on the existing RRULE if user provided one.
        QDate newUntil = fields.value(QStringLiteral("recurrenceUntil")).toDate();
        if (newUntil != master.recurrenceUntil) {
            // Read existing rule, modify UNTIL, write back
           icalproperty* rp = icalcomponent_get_first_property(ve, ICAL_RRULE_PROPERTY);
            if (rp) {
                struct icalrecurrencetype rule = icalproperty_get_rrule(rp);  // by value
                if (newUntil.isValid()) {
                    icaltimetype u = icaltime_null_date();
                    u.year = newUntil.year(); u.month = newUntil.month(); u.day = newUntil.day();
                    u.is_date = 1;
                    rule.until = u;
                } else {
                    rule.until = icaltime_null_time();
                }
                icalproperty_set_rrule(rp, rule);
            }
        }
    }

    // Bump SEQUENCE
    int seq = icalcomponent_get_sequence(ve);
    icalcomponent_set_sequence(ve, seq + 1);

    // Update LAST-MODIFIED
    icalproperty* lm = icalcomponent_get_first_property(ve, ICAL_LASTMODIFIED_PROPERTY);
    icaltimetype now = icaltime_current_time_with_zone(icaltimezone_get_utc_timezone());
    if (lm) icalproperty_set_lastmodified(lm, now);
    else icalcomponent_add_property(ve, icalproperty_new_lastmodified(now));

    const QString text = IcalParser::serializeComponent(root.get());
    if (text.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Serialise failed");
        return false;
    }
    return Atomic::writeFile(master.sourceFile, text.toUtf8(), errorOut);
}

bool CalendarStore::editSingleOverride(const Event& master, const QVariantMap& fields,
                                       const QDateTime& occurrenceStart, QString* errorOut)
{
    // Step 1 — add EXDATE to master file
    if (!deleteSingleOccurrence(master, occurrenceStart, errorOut)) return false;

    // Step 2 — write override file (new VEVENT with same UID + RECURRENCE-ID)
    QVariantMap fx = fields;
    fx.insert(QStringLiteral("uid"), master.uid);
    fx.insert(QStringLiteral("recurrenceId"), occurrenceStart);
    fx.insert(QStringLiteral("recurrence"), QStringLiteral("none"));
    fx.remove(QStringLiteral("recurrenceUntil"));

    const QString text = IcalParser::buildIcsString(fx);
    if (text.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Build failed for override");
        return false;
    }
    const QString uniqueUid = master.uid + QStringLiteral("-override-")
                            + occurrenceStart.toString(QStringLiteral("yyyyMMddhhmm"));
    const QString path = newIcsPath(master.calendar, uniqueUid);
    if (path.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Calendar path not resolvable");
        return false;
    }
    return Atomic::writeFile(path, text.toUtf8(), errorOut);
}

bool CalendarStore::deleteWholeSeries(const Event& master, QString* errorOut) {
    if (!QFile::remove(master.sourceFile)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot delete ") + master.sourceFile;
        return false;
    }
    return true;
}

bool CalendarStore::deleteSingleOccurrence(const Event& master,
                                           const QDateTime& occurrenceStart,
                                           QString* errorOut)
{
    QFile in(master.sourceFile);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot read ") + master.sourceFile;
        return false;
    }
    QByteArray data = in.readAll();
    in.close();

    ical::ComponentPtr root = IcalParser::parseToComponent(data);
    if (!root) {
        if (errorOut) *errorOut = QStringLiteral("Parse error in ") + master.sourceFile;
        return false;
    }
    icalcomponent* ve = findMasterVevent(root.get(), master.uid);
    if (!ve) {
        if (errorOut) *errorOut = QStringLiteral("Master VEVENT not found");
        return false;
    }
    icaltimetype ex = IcalParser::qDateTimeToIcalTime(occurrenceStart, master.allDay);
    icalcomponent_add_property(ve, icalproperty_new_exdate(ex));

    const QString text = IcalParser::serializeComponent(root.get());
    if (text.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Serialise failed");
        return false;
    }
    return Atomic::writeFile(master.sourceFile, text.toUtf8(), errorOut);
}

// ── FSW handling ──────────────────────────────────────────────────────

void CalendarStore::handleFsEvent(const QString& path) {
    Q_UNUSED(path);
    if (QDateTime::currentMSecsSinceEpoch() < m_ignoreFsUntil) return;
    m_debounce->start();
}

void CalendarStore::debouncedRescan() {
    rescanAll();
}

void CalendarStore::markSelfMutated() {
    m_ignoreFsUntil = QDateTime::currentMSecsSinceEpoch() + 500;
}

void CalendarStore::rebuildWatchPaths() {
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());

    QStringList dirs;
    for (const auto& cal : m_calendars) {
        dirs << cal.path;
        const QDir d(cal.path);
        const auto sub = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& fi : sub) dirs << fi.absoluteFilePath();
    }
    if (!dirs.isEmpty()) m_watcher->addPaths(dirs);
}

// ── Calendar discovery ────────────────────────────────────────────────

std::vector<CalendarStore::CalendarInfo> CalendarStore::discoverCalendars() {
    std::vector<CalendarInfo> out;
    QFile f(khalConfigPath());

    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        QString currentName;
        QString currentPath;
        QString currentType = QStringLiteral("calendar");
        bool inSection = false;
        bool inCalendars = false;

        auto flush = [&]() {
            if (!currentName.isEmpty() && !currentPath.isEmpty()) {
                QString p = expandTilde(currentPath);
                if (p.endsWith(QStringLiteral("/*"))) p.chop(2);
                if (currentType == QLatin1String("discover")) {
                    QDir d(p);
                    const auto subs = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const QFileInfo& fi : subs) {
                        out.push_back({ currentName + QLatin1Char('/') + fi.fileName(),
                                        fi.absoluteFilePath() });
                    }
                } else {
                    out.push_back({ currentName, p });
                }
            }
            currentName.clear();
            currentPath.clear();
            currentType = QStringLiteral("calendar");
            inSection = false;
        };

        while (!ts.atEnd()) {
            QString line = ts.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
                if (inSection) flush();
                QString header = line.mid(1, line.size() - 2);
                if (header.startsWith(QLatin1Char('[')) && header.endsWith(QLatin1Char(']'))) {
                    if (inCalendars) {
                        currentName = header.mid(1, header.size() - 2);
                        inSection = true;
                    }
                } else {
                    inCalendars = (header == QLatin1String("calendars"));
                    if (inSection) flush();
                }
                continue;
            }
            if (!inSection) continue;
            int eq = line.indexOf(QLatin1Char('='));
            if (eq < 0) continue;
            QString key = line.left(eq).trimmed();
            QString val = cleanIniValue(line.mid(eq + 1));
            if (key == QLatin1String("path")) currentPath = val;
            else if (key == QLatin1String("type")) currentType = val;
        }
        if (inSection) flush();
        f.close();
    }

    if (out.empty()) {
        // Filesystem fallback
        QDir root(defaultStorageRoot());
        if (root.exists()) {
            const auto accounts = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QFileInfo& acc : accounts) {
                QDir adir(acc.absoluteFilePath());
                const auto cols = adir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
                if (cols.isEmpty()) {
                    out.push_back({ acc.fileName(), acc.absoluteFilePath() });
                } else {
                    for (const QFileInfo& col : cols)
                        out.push_back({ acc.fileName() + QLatin1Char('/') + col.fileName(),
                                        col.absoluteFilePath() });
                }
            }
        }
    }

    return out;
}
