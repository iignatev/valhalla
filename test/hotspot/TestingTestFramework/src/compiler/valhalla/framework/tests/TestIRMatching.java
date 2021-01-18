package compiler.valhalla.framework.tests;

import compiler.valhalla.framework.*;
import jdk.test.lib.Asserts;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Predicate;
import java.util.regex.Pattern;

public class TestIRMatching {
    static int[] testExecuted = new int[61];

    public static void main(String[] args) {
        // Run with -DPrintValidIRRules=true to simulate TestVM
        runFailOnTests(Constraint.failOnNodes(AndOr1.class, "test1(int)", 1, true,"CallStaticJava"), "-XX:SuspendRetryCount=50", "-XX:+UsePerfData", "-XX:+UseTLAB");
        runFailOnTests(Constraint.failOnNodes(AndOr1.class, "test2()", 1, true,"CallStaticJava"), "-XX:SuspendRetryCount=50", "-XX:-UsePerfData", "-XX:+UseTLAB");

        runWithArguments(AndOr1.class, "-XX:SuspendRetryCount=52", "-XX:+UsePerfData", "-XX:+UseTLAB");

        runWithArguments(Comparisons.class, "-XX:SuspendRetryCount=50");
        findIrIds(TestFramework.getLastVmOutput(), "testMatchAllIf50", 0, 21);
        findIrIds(TestFramework.getLastVmOutput(), "testMatchNoneIf50", -1, -1);

        runWithArguments(Comparisons.class, "-XX:SuspendRetryCount=49");
        findIrIds(TestFramework.getLastVmOutput(), "testMatchAllIf50", 4, 6, 13, 18);
        findIrIds(TestFramework.getLastVmOutput(), "testMatchNoneIf50", 0, 3, 8, 10, 17, 22);

        runWithArguments(Comparisons.class, "-XX:SuspendRetryCount=51");
        findIrIds(TestFramework.getLastVmOutput(), "testMatchAllIf50", 7, 12, 19, 21);
        findIrIds(TestFramework.getLastVmOutput(), "testMatchNoneIf50", 4, 7, 11, 16, 20, 22);

        runWithArguments(MultipleFailOnGood.class, "-XX:SuspendRetryCount=50");

        runFailOnTests(Constraint.failOnNodes(MultipleFailOnBad.class, "fail1()", 1,true, "Store"),
                       Constraint.failOnNodes(MultipleFailOnBad.class, "fail2()", 1,true, "CallStaticJava"),
                       Constraint.failOnNodes(MultipleFailOnBad.class, "fail3()", 1,true, "Store"),
                       Constraint.failOnNodes(MultipleFailOnBad.class, "fail4()", 1,true, "Store"),
                       Constraint.failOnMatches(MultipleFailOnBad.class, "fail5()", 1,true, "Store", "iFld"),
                       Constraint.failOnAlloc(MultipleFailOnBad.class, "fail6()", 1,true, "MyClass"),
                       Constraint.failOnAlloc(MultipleFailOnBad.class, "fail7()", 1,true, "MyClass"),
                       Constraint.failOnAlloc(MultipleFailOnBad.class, "fail8()", 1,true, "MyClass"),
                       Constraint.failOnNodes(MultipleFailOnBad.class, "fail9()", 1,true, "Store", "CallStaticJava"),
                       Constraint.failOnMatches(MultipleFailOnBad.class, "fail10()", 1,true, "Store", "iFld"));

        runWithArguments(GoodCount.class, "-XX:SuspendRetryCount=50");

        runFailOnTests(Constraint.failOnArrayAlloc(VariousIrNodes.class, "allocArray()", 1,true, "MyClass"),
                       Constraint.failOnArrayAlloc(VariousIrNodes.class, "allocArray()", 2,true, "MyClass"),
                       Constraint.failOnArrayAlloc(VariousIrNodes.class, "allocArray()", 3,false, "MyClass"),
                       Constraint.failOnArrayAlloc(VariousIrNodes.class, "allocArray()", 4,false, "MyClass"),
                       Constraint.failOnArrayAlloc(VariousIrNodes.class, "allocArray()", 5,true, "MyClass"),
                       Constraint.failOnNodes(VariousIrNodes.class, "loop()", 1, true, "Loop"),
                       Constraint.failOnNodes(VariousIrNodes.class, "loop()", 2, false, "CountedLoop"),
                       Constraint.failOnNodes(VariousIrNodes.class, "countedLoop()", 1, false, "Loop"),
                       Constraint.failOnNodes(VariousIrNodes.class, "countedLoop()", 2, true, "CountedLoop"),
                       Constraint.failOnNodes(VariousIrNodes.class, "loopAndCountedLoop()", 1, true, "Loop"),
                       Constraint.failOnNodes(VariousIrNodes.class, "loopAndCountedLoop()", 2, true, "CountedLoop"),
                       Constraint.failOnNodes(VariousIrNodes.class, "load()", 1, true, "Load"),
                       Constraint.failOnNodes(VariousIrNodes.class, "load()", 2, true, "VariousIrNodes"),
                       Constraint.failOnNodes(VariousIrNodes.class, "load()", 3, true, "VariousIrNodes"),
                       Constraint.failOnMatches(VariousIrNodes.class, "load()", 4, true, "Load", "iFld"),
                       Constraint.failOnNodes(VariousIrNodes.class, "load()", 5, false, "Load")
        );

    }

    private static void runWithArguments(Class<?> clazz, String... args) {
        TestFramework.runWithScenarios(clazz, new Scenario(0, args));
    }

    private static void runFailOnTests(Constraint... constraints) {
        try {
            TestFramework.run(constraints[0].getKlass()); // All constraints have the same class.
            shouldNotReach();
        } catch (ShouldNotReachException e) {
            throw e;
        } catch (RuntimeException e) {
            System.out.println(e.getMessage());
            for (Constraint constraint : constraints) {
                constraint.checkConstraint(e);
            }
        }
    }

    // Single constraint
    private static void runFailOnTests(Constraint constraint, String... args) {
        try {
            Scenario scenario = new Scenario(0, args);
            TestFramework.runWithScenarios(constraint.getKlass(), scenario); // All constraints have the same class.
            shouldNotReach();
        } catch (ShouldNotReachException e) {
           throw e;
        } catch (TestRunException e) {
            System.out.println(e.getMessage());
            constraint.checkConstraint(e);
        }
    }

    private static void shouldNotReach() {
        throw new ShouldNotReachException("Framework did not fail but it should have!");
    }

    public static void findIrIds(String output, String method, int... numbers) {
        StringBuilder builder = new StringBuilder();
        builder.append(method);
        for (int i = 0; i < numbers.length; i+=2) {
            int start = numbers[i];
            int endIncluded = numbers[i + 1];
            for (int j = start; j <= endIncluded; j++) {
                builder.append(",");
                builder.append(j);
            }
        }
        Asserts.assertTrue(output.contains(builder.toString()), "Could not find encoding: \"" + builder.toString() + "\n");
    }
}

class AndOr1 {
    @Test
    @Arguments(ArgumentValue.DEFAULT)
    @IR(applyIfAnd={"UsePerfData", "true", "SuspendRetryCount", "50", "UseTLAB", "true"}, failOn={IRNode.CALL})
    public void test1(int i) {
        dontInline();
    }

    @Test
    @IR(applyIfOr={"UsePerfData", "false", "SuspendRetryCount", "51", "UseTLAB", "false"}, failOn={IRNode.CALL})
    public void test2() {
        dontInline();
    }

    @DontInline
    private void dontInline() {
    }
}

class MultipleFailOnGood {
    private int iFld;
    private MyClassSub myClassSub = new MyClassSub();

    @Test
    @IR(applyIf={"SuspendRetryCount", "50"}, failOn={IRNode.STORE, IRNode.CALL})
    @IR(failOn={IRNode.STORE, IRNode.CALL})
    @IR(applyIfOr={"SuspendRetryCount", "99", "SuspendRetryCount", "100"}, failOn={IRNode.RETURN, IRNode.CALL}) // Not applied
    public void good1() {
        forceInline();
    }

    @Test
    @IR(failOn={IRNode.STORE, IRNode.CALL})
    @IR(applyIfNot={"SuspendRetryCount", "20"}, failOn={IRNode.ALLOC})
    @IR(applyIfNot={"SuspendRetryCount", "< 100"}, failOn={IRNode.ALLOC_OF, "Test"})
    public void good2() {
        forceInline();
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_CLASS, "Test", IRNode.CALL})
    @IR(applyIfNot={"SuspendRetryCount", "20"}, failOn={IRNode.ALLOC})
    @IR(applyIfNot={"SuspendRetryCount", "< 100"}, failOn={IRNode.ALLOC_OF, "Test"})
    public void good3() {
        forceInline();
    }

    @Test
    @IR(failOn={IRNode.CALL, IRNode.STORE_OF_CLASS, "UnknownClass"})
    public void good4() {
        iFld = 42;
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_FIELD, "xFld", IRNode.CALL})
    public void good5() {
        iFld = 42;
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_CLASS, "MyClass"}) // Needs exact match to fail
    public void good6() {
        myClassSub.iFld = 42;
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_CLASS, "MyClassSub"}) // Static write is with Class and not MySubClass
    public void good7() {
        MyClassSub.iFldStatic = 42;
    }

    @ForceInline
    private void forceInline() {
    }
}

class MultipleFailOnBad {
    private int iFld;
    private int myInt;
    private MyClass myClass;
    @Test
    @IR(failOn={IRNode.STORE, IRNode.CALL})
    public void fail1() {
        iFld = 42;
    }

    @Test
    @IR(failOn={IRNode.STORE, IRNode.CALL})
    public void fail2() {
        dontInline();
    }

    @Test
    @IR(failOn={IRNode.CALL, IRNode.STORE_OF_CLASS, "MultipleFailOnBad", IRNode.ALLOC})
    public void fail3() {
        iFld = 42;
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_CLASS, "compiler/valhalla/framework/tests/MultipleFailOnBad", IRNode.CALL, IRNode.ALLOC})
    public void fail4() {
        iFld = 42;
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_FIELD, "iFld", IRNode.CALL, IRNode.ALLOC})
    public void fail5() {
        iFld = 42;
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_CLASS, "MyClass", IRNode.ALLOC})
    public void fail6() {
        myClass = new MyClass();
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_CLASS, "UnknownClass", IRNode.ALLOC_OF, "MyClass"})
    public void fail7() {
        myClass = new MyClass();
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_CLASS, "UnknownClass", IRNode.ALLOC_OF, "compiler/valhalla/framework/tests/MyClassSub"})
    public void fail8() {
        myClass = new MyClassSub();
    }

    @Test
    @IR(failOn={IRNode.STORE, IRNode.CALL})
    public void fail9() {
        iFld = 42;
        dontInline();
    }

    @Test
    @IR(failOn={IRNode.STORE_OF_FIELD, "iFld", IRNode.CALL, IRNode.ALLOC})
    public void fail10() {
        myInt = 34;
        iFld = 42;
    }

    @DontInline
    private void dontInline() {
    }
}

// Called with -XX:SuspendRetryCount=X.
class Comparisons {
    // Applies all IR rules if SuspendRetryCount=50
    @Test
    @IR(applyIf={"SuspendRetryCount", "50"}) // Index 0
    @IR(applyIf={"SuspendRetryCount", "=50"})
    @IR(applyIf={"SuspendRetryCount", "= 50"})
    @IR(applyIf={"SuspendRetryCount", " =  50"})
    @IR(applyIf={"SuspendRetryCount", "<=50"}) // Index 4
    @IR(applyIf={"SuspendRetryCount", "<= 50"})
    @IR(applyIf={"SuspendRetryCount", " <=  50"})
    @IR(applyIf={"SuspendRetryCount", ">=50"}) // Index 7
    @IR(applyIf={"SuspendRetryCount", ">= 50"})
    @IR(applyIf={"SuspendRetryCount", " >=  50"})
    @IR(applyIf={"SuspendRetryCount", ">49"})
    @IR(applyIf={"SuspendRetryCount", "> 49"})
    @IR(applyIf={"SuspendRetryCount", " >  49"})
    @IR(applyIf={"SuspendRetryCount", "<51"}) // Index 13
    @IR(applyIf={"SuspendRetryCount", "< 51"})
    @IR(applyIf={"SuspendRetryCount", " <  51"})
    @IR(applyIf={"SuspendRetryCount", "!=51"})
    @IR(applyIf={"SuspendRetryCount", "!= 51"})
    @IR(applyIf={"SuspendRetryCount", " !=  51"})
    @IR(applyIf={"SuspendRetryCount", "!=49"})
    @IR(applyIf={"SuspendRetryCount", "!= 49"})
    @IR(applyIf={"SuspendRetryCount", " !=  49"}) // Index 21
    public void testMatchAllIf50() {
    }

    // Applies no IR rules if SuspendRetryCount=50
    @Test
    @IR(applyIf={"SuspendRetryCount", "49"}) // Index 0
    @IR(applyIf={"SuspendRetryCount", "=49"})
    @IR(applyIf={"SuspendRetryCount", "= 49"})
    @IR(applyIf={"SuspendRetryCount", " =  49"})
    @IR(applyIf={"SuspendRetryCount", "51"}) // Index 4
    @IR(applyIf={"SuspendRetryCount", "=51"})
    @IR(applyIf={"SuspendRetryCount", "= 51"})
    @IR(applyIf={"SuspendRetryCount", " =  51"})
    @IR(applyIf={"SuspendRetryCount", "<=49"}) // Index 8
    @IR(applyIf={"SuspendRetryCount", "<= 49"})
    @IR(applyIf={"SuspendRetryCount", " <=  49"})
    @IR(applyIf={"SuspendRetryCount", ">=51"}) // Index 11
    @IR(applyIf={"SuspendRetryCount", ">= 51"})
    @IR(applyIf={"SuspendRetryCount", " >=  51"})
    @IR(applyIf={"SuspendRetryCount", ">50"})
    @IR(applyIf={"SuspendRetryCount", "> 50"})
    @IR(applyIf={"SuspendRetryCount", " >  50"})
    @IR(applyIf={"SuspendRetryCount", "<50"}) // Index 17
    @IR(applyIf={"SuspendRetryCount", "< 50"})
    @IR(applyIf={"SuspendRetryCount", " <  50"})
    @IR(applyIf={"SuspendRetryCount", "!=50"})
    @IR(applyIf={"SuspendRetryCount", "!= 50"})
    @IR(applyIf={"SuspendRetryCount", " !=  50"}) // Index 22
    public void testMatchNoneIf50() {
    }
}


class GoodCount {
    int iFld;
    int iFld2;
    MyClass myClass = new MyClass();

    @Test
    @IR(applyIf={"SuspendRetryCount", "50"}, counts={IRNode.STORE, "1"})
    public void good1() {
        iFld = 3;
    }

    @Test
    @IR(counts={IRNode.STORE, "2"})
    public void good2() {
        iFld = 3;
        iFld2 = 4;
    }

    @Test
    @IR(counts={IRNode.STORE, "2", IRNode.STORE_OF_CLASS, "GoodCount", "2"})
    public void good3() {
        iFld = 3;
        iFld2 = 4;
    }

    @Test
    @IR(counts={IRNode.STORE_OF_FIELD, "iFld", "1", IRNode.STORE, "2", IRNode.STORE_OF_CLASS, "GoodCount", "2"})
    public void good4() {
        iFld = 3;
        iFld2 = 4;
    }
}

// Test on remaining IR nodes that we have not tested above, yet.
class VariousIrNodes {
    MyClass[] myClassArray;
    int limit = 1024;
    int iFld = 34;
    int result = 0;
    @Test
    @IR(failOn={IRNode.ALLOC_ARRAY})
    @IR(failOn={IRNode.ALLOC_ARRAY_OF, "MyClass"})
    @IR(failOn={IRNode.ALLOC_ARRAY_OF, "MyClasss"}) // Does not fail
    @IR(failOn={IRNode.ALLOC_ARRAY_OF, "compiler/valhalla/framework/tests/MySubClass"}) // Does not fail
    @IR(failOn={IRNode.ALLOC_ARRAY_OF, "compiler/valhalla/framework/tests/MyClass"})
    public void allocArray() {
        myClassArray = new MyClass[2];
    }

    @Test
    @IR(failOn={IRNode.LOOP})
    @IR(failOn={IRNode.COUNTEDLOOP}) // Does not fail
    public void loop() {
        for (int i = 0; i < limit; i++) {
            dontInline();
        }
    }

    @Test
    @IR(failOn={IRNode.LOOP}) // Does not fail
    @IR(failOn={IRNode.COUNTEDLOOP})
    public void countedLoop() {
        for (int i = 0; i < 2000; i++) {
            dontInline();
        }
    }

    @Test
    @IR(failOn={IRNode.LOOP})
    @IR(failOn={IRNode.COUNTEDLOOP})
    public void loopAndCountedLoop() {
        for (int i = 0; i < 2000; i++) {
            for (int j = 0; j < limit; j++) {
                dontInline();
            }
        }
    }

    @DontInline
    public void dontInline() {}

    @Test
    @IR(failOn={IRNode.LOAD})
    @IR(failOn={IRNode.LOAD_OF_CLASS, "compiler/valhalla/framework/tests/VariousIrNodes"})
    @IR(failOn={IRNode.LOAD_OF_CLASS, "VariousIrNodes"})
    @IR(failOn={IRNode.LOAD_OF_FIELD, "iFld"})
    @IR(failOn={IRNode.LOAD_OF_FIELD, "iFld2", IRNode.LOAD_OF_CLASS, "Various"}) // Does not fail
    public void load() {
        result = iFld;
    }

    @Test
    @IR(failOn={IRNode.RETURN})
    public void returns() {
        dontInline();
    }

}

class MyClass {
    int iFld;
}
class MyClassSub extends MyClass {
    int iFld;
    static int iFldStatic;
}

enum FailType {
    FAIL_ON
}

class Constraint {
    final private Class<?> klass;
    final private int ruleIdx;
    final private Pattern irPattern;
    final private List<String> matches;
    final private Pattern methodPattern;
    private final String classAndMethod;
    final FailType failType;
    final boolean shouldMatch;

    private Constraint(Class<?> klass, String methodName, int ruleIdx, FailType failType, List<String> matches, boolean shouldMatch) {
        this.klass = klass;
        classAndMethod = klass.getSimpleName() + "." + methodName;
        this.ruleIdx = ruleIdx;
        this.failType = failType;
        this.methodPattern = Pattern.compile(Pattern.quote(classAndMethod));
        if (failType == FailType.FAIL_ON) {
            irPattern = Pattern.compile("rule " + ruleIdx + ":.*\\R.*Failure:.*contains forbidden node:");
        } else {
            irPattern = null; // TODO
        }
        this.shouldMatch = shouldMatch;
        this.matches = matches;
    }

    public Class<?> getKlass() {
        return klass;
    }

    public static Constraint failOnNodes(Class<?> klass, String methodName, int ruleIdx, boolean shouldMatch, String... nodes) {
        return new Constraint(klass, methodName, ruleIdx, FailType.FAIL_ON, new ArrayList<>(Arrays.asList(nodes)), shouldMatch);
    }

    public static Constraint failOnAlloc(Class<?> klass, String methodName, int ruleIdx, boolean shouldMatch, String allocKlass) {
        List<String> list = new ArrayList<>();
        list.add(allocKlass);
        list.add("call,static  wrapper for: _new_instance_Java");
        return new Constraint(klass, methodName, ruleIdx, FailType.FAIL_ON, list, shouldMatch);
    }

    public static Constraint failOnArrayAlloc(Class<?> klass, String methodName, int ruleIdx, boolean shouldMatch, String allocKlass) {
        List<String> list = new ArrayList<>();
        list.add(allocKlass);
        list.add("call,static  wrapper for: _new_array_Java");
        return new Constraint(klass, methodName, ruleIdx, FailType.FAIL_ON, list, shouldMatch);
    }

    public static Constraint failOnMatches(Class<?> klass, String methodName, int ruleIdx, boolean shouldMatch, String... matches) {
        return new Constraint(klass, methodName, ruleIdx, FailType.FAIL_ON, new ArrayList<>(Arrays.asList(matches)), shouldMatch);
    }

    public void checkConstraint(RuntimeException e) {
        String message = e.getMessage();
        String[] splitMethods = message.split("Method");
        for (String method : splitMethods) {
            if (methodPattern.matcher(method).find()) {
                String[] splitIrRules = method.split("@IR");
                for (String irRule : splitIrRules) {
                    if (irPattern.matcher(irRule).find()) {
                        boolean allMatch = matches.stream().allMatch(irRule::contains);
                        if (shouldMatch) {
                            Asserts.assertTrue(allMatch, "Constraint for method " + classAndMethod + ", rule " + ruleIdx + " could not be matched:\n" + message);
                        } else {
                            Asserts.assertFalse(allMatch, "Constraint for method " + classAndMethod  + ", rule " + ruleIdx + " should not have been matched:\n" + message);
                        }
                        return;
                    }
                }
                Predicate<String> irPredicate = s -> irPattern.matcher(s).find();
                if (shouldMatch) {
                    Asserts.assertTrue(Arrays.stream(splitIrRules).anyMatch(irPredicate), "Constraint for method " + classAndMethod + ", rule "
                                       + ruleIdx + " could not be matched:\n" + message);
                } else {
                    Asserts.assertTrue(Arrays.stream(splitIrRules).noneMatch(irPredicate), "Constraint for method " + classAndMethod + ", rule "
                                       + ruleIdx + " should not have been matched:\n" + message);
                }
                return;
            }
        }
        if (shouldMatch) {
            Asserts.fail("Constraint for method " + classAndMethod + ", rule " + ruleIdx + " could not be matched:\n" + message);
        }
    }
}

class ShouldNotReachException extends RuntimeException {
    ShouldNotReachException(String s) {
        super(s);
    }
}
