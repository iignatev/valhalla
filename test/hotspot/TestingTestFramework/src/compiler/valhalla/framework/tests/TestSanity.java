/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

import compiler.valhalla.framework.Scenario;
import compiler.valhalla.framework.Test;
import compiler.valhalla.framework.TestFramework;

import java.util.ArrayList;

public class TestSanity {

    public static void main(String[] args) {
        TestFramework.run();
        TestFramework.run(TestSanity.class);
        TestFramework.runWithHelperClasses(TestSanity.class, HelperA.class);
        TestFramework.runWithHelperClasses(TestSanity.class, HelperA.class, HelperB.class);
        Scenario sDefault = new Scenario(0);
        Scenario s1 = new Scenario(1, "-XX:SuspendRetryCount=52", "-XX:+UseTLAB");
        Scenario s2 = new Scenario(2, "-XX:SuspendRetryCount=53", "-XX:+UseTLAB");
        TestFramework.runWithScenarios(s1);
        TestFramework.runWithScenarios(s1, s2);
        TestFramework.runWithScenarios(TestSanity.class, s1, s2);
        TestFramework.runWithScenarios(sDefault, s1);
        TestFramework.runWithScenarios(sDefault, s1, s2);
        TestFramework.runWithScenarios(TestSanity.class, sDefault, s1);
        TestFramework.runWithScenarios(TestSanity.class, sDefault, s1, s2);
        TestFramework testFramework = new TestFramework(TestSanity.class);
        testFramework.start();
        testFramework.addHelperClasses(HelperA.class, HelperB.class).start();
        testFramework.clearHelperClasses();
        testFramework.addHelperClasses(HelperA.class, HelperB.class).addHelperClasses(HelperC.class).start();
        testFramework.clearHelperClasses();
        testFramework.addScenarios(sDefault).addScenarios(s1, s2).start();
        testFramework.clearScenarios();
        testFramework.addHelperClasses(HelperA.class).addScenarios(sDefault).start();
        testFramework.clear();
        testFramework.addHelperClasses(HelperA.class).addScenarios(sDefault).addHelperClasses(HelperB.class, HelperC.class)
                     .addScenarios(s1, s2).start();
    }

    @Test
    public void test() {
    }
}

class HelperA { }
class HelperB { }
class HelperC { }
