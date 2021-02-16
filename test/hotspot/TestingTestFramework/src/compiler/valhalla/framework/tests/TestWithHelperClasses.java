/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package compiler.valhalla.framework.tests;

import compiler.valhalla.framework.*;
import jdk.test.lib.Asserts;

public class TestWithHelperClasses {

    public static void main(String[] args) {
        TestFramework.runWithHelperClasses(TestWithHelperClasses.class, Helper1.class, Helper2.class);
        try {
            TestFramework.runWithHelperClasses(TestWithHelperClasses.class, Helper1.class);
        } catch (Exception e) {
            Asserts.assertFalse(TestFramework.getLastVmOutput().contains("public static void compiler.valhalla.framework.tests.Helper1.foo() should have been C2 compiled"));
            Asserts.assertTrue(TestFramework.getLastVmOutput().contains("public static void compiler.valhalla.framework.tests.Helper2.foo() should have been C2 compiled"));
            return;
        }
        throw new RuntimeException("Did not catch exception");
    }

    @Test
    public void test() throws NoSuchMethodException {
        TestFramework.assertCompiledByC2(Helper1.class.getMethod("foo"));
        TestFramework.assertCompiledByC2(Helper2.class.getMethod("foo"));
    }
}

class Helper1 {

    @ForceCompile(CompLevel.C2)
    public static void foo() {
        throw new RuntimeException("Should not be executed");
    }
}

class Helper2 {

    @ForceCompile(CompLevel.C2)
    public static void foo() {
        throw new RuntimeException("Should not be executed");
    }
}
