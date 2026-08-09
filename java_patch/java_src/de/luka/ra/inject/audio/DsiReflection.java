package de.luka.ra.inject.audio;

import java.lang.reflect.Method;
import java.util.List;

/**
 * Java 1.4-compatible lookup for services hidden behind LSD class loaders.
 * Once found, AudioFocusBridge uses the exact MU1316 interfaces directly.
 */
final class DsiReflection {
    private static ClassLoader cachedLsdClassLoader;

    private DsiReflection() {}

    static Object getService(String serviceName, ClassLoader hint) {
        if (serviceName == null) return null;
        try {
            Object registry = getRegistry(hint);
            if (registry == null) return null;
            Class registryClass = registry.getClass();
            Method getInfo = registryClass.getMethod(
                    "getServiceInfo", new Class[]{String.class});
            Object info = getInfo.invoke(registry, new Object[]{serviceName});
            if (info == null) return null;
            Method getService = info.getClass().getMethod(
                    "getService", new Class[0]);
            return getService.invoke(info, new Object[0]);
        } catch (Throwable ignored) {
            return null;
        }
    }

    /** Select one of LSD's same-interface services by its OSGi property. */
    static Object getServiceByProperty(String serviceName, String propertyName,
                                       Object propertyValue, ClassLoader hint) {
        if (serviceName == null || propertyName == null) return null;
        try {
            Object registry = getRegistry(hint);
            if (registry == null) return null;
            Method getInfos = registry.getClass().getMethod(
                    "getServiceInfos", new Class[]{String.class});
            List infos = (List)getInfos.invoke(registry,
                    new Object[]{serviceName});
            if (infos == null) return null;

            for (int i = 0; i < infos.size(); i++) {
                Object info = infos.get(i);
                if (info == null) continue;
                Method getProperty = info.getClass().getMethod(
                        "getProperty", new Class[]{String.class});
                Object value = getProperty.invoke(info,
                        new Object[]{propertyName});
                if (propertyValue == null ? value == null
                        : propertyValue.equals(value)) {
                    Method getService = info.getClass().getMethod(
                            "getService", new Class[0]);
                    return getService.invoke(info, new Object[0]);
                }
            }
        } catch (Throwable ignored) {}
        return null;
    }

    static ClassLoader getLsdClassLoader(ClassLoader hint) {
        if (cachedLsdClassLoader == null) {
            cachedLsdClassLoader = findClassLoader(
                    "de.dreisoft.lsd.ServiceRegistry", hint);
        }
        return cachedLsdClassLoader;
    }

    private static Object getRegistry(ClassLoader hint) throws Exception {
        ClassLoader loader = getLsdClassLoader(hint);
        if (loader == null) return null;

        Class registryClass = Class.forName(
                "de.dreisoft.lsd.ServiceRegistry", true, loader);
        Method getInstance;
        try {
            getInstance = registryClass.getMethod("getInstance", new Class[0]);
        } catch (NoSuchMethodException e) {
            getInstance = registryClass.getDeclaredMethod(
                    "getInstance", new Class[0]);
            getInstance.setAccessible(true);
        }
        return getInstance.invoke(null, new Object[0]);
    }

    private static ClassLoader findClassLoader(String className, ClassLoader hint) {
        ClassLoader[] quick = new ClassLoader[3];
        quick[0] = hint;
        try { quick[1] = Thread.currentThread().getContextClassLoader(); }
        catch (Throwable ignored) { quick[1] = null; }
        try { quick[2] = ClassLoader.getSystemClassLoader(); }
        catch (Throwable ignored) { quick[2] = null; }

        for (int i = 0; i < quick.length; i++) {
            if (quick[i] == null) continue;
            try {
                Class.forName(className, false, quick[i]);
                return quick[i];
            } catch (Throwable ignored) {}
        }

        try {
            ThreadGroup root = Thread.currentThread().getThreadGroup();
            while (root.getParent() != null) root = root.getParent();
            Thread[] threads = new Thread[root.activeCount() + 50];
            int count = root.enumerate(threads, true);
            for (int i = 0; i < count; i++) {
                if (threads[i] == null) continue;
                try {
                    ClassLoader loader = threads[i].getContextClassLoader();
                    if (loader == null) continue;
                    Class.forName(className, false, loader);
                    return loader;
                } catch (Throwable ignored) {}
            }
        } catch (Throwable ignored) {}
        return null;
    }
}
