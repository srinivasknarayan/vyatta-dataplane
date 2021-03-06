#!groovy

// Pull Request builds might fail due to missing diffs: https://issues.jenkins-ci.org/browse/JENKINS-45997
// Pull Request builds relationship to their targets branch: https://issues.jenkins-ci.org/browse/JENKINS-37491

@NonCPS
def cancelPreviousBuilds() {
    def jobName = env.JOB_NAME
    def buildNumber = env.BUILD_NUMBER.toInteger()
    /* Get job name */
    def currentJob = Jenkins.instance.getItemByFullName(jobName)

    /* Iterating over the builds for specific job */
    for (def build : currentJob.builds) {
        /* If there is a build that is currently running and it's not current build */
        if (build.isBuilding() && build.number.toInteger() != buildNumber) {
            /* Than stopping it */
            build.doStop()
        }
    }
}

pipeline {
    agent any

    environment {
	OBS_TARGET_PROJECT = 'DANOS:Master'
	OBS_TARGET_REPO = 'standard'
	OBS_TARGET_ARCH = 'x86_64'
	// # Replace : with _ in project name, as osc-buildpkg does
	BUILD_ROOT_RELATIVE = 'build-root/' + "${env.OBS_TARGET_PROJECT.replace(':','_')}" + '-' + "${env.OBS_TARGET_REPO}" + '-' + "${OBS_TARGET_ARCH}"
	OSC_BUILD_ROOT = "${WORKSPACE}" + '/' + "${env.BUILD_ROOT_RELATIVE}"
	// CHANGE_TARGET is set for PRs.
	// When CHANGE_TARGET is not set it's a regular build so we use BRANCH_NAME.
	REF_BRANCH = "${env.CHANGE_TARGET != null ? env.CHANGE_TARGET : env.BRANCH_NAME}"
	DH_VERBOSE = 1
	DH_QUIET = 0
	DEB_BUILD_OPTIONS ='verbose all_tests sanitizer'
    }

    options {
	timeout(time: 60, unit: 'MINUTES')
	checkoutToSubdirectory("vyatta-dataplane")
	quietPeriod(90) // Wait 90 seconds in case there are more SCM pushes/PR merges coming
    }

    stages {

	// A work around, until this feature is implemented: https://issues.jenkins-ci.org/browse/JENKINS-47503
	stage('Cancel older builds') { steps { script {
	    cancelPreviousBuilds()
        }}}

	stage('OSC config') {
	    steps {
		sh 'printenv'
		// Build scripts with tasks to perform in the chroot
		sh """
cat <<EOF > osc-buildpackage_buildscript_default
export BUILD_ID=\"${BUILD_ID}\"
export JENKINS_NODE_COOKIE=\"${JENKINS_NODE_COOKIE}\"
dpkg-buildpackage -jauto -us -uc -b
EOF
"""
	    }
	}

	// Workspace specific chroot location used instead of /var/tmp
	// Allows parallel builds between jobs, but not between stages in a single job
	// TODO: Enhance osc-buildpkg to support parallel builds from the same pkg_srcdir
	// TODO: probably by allowing it to accept a .conf file from a location other than pkg_srcdir

	stage('OSC Build') {
	    steps {
		dir('vyatta-dataplane') {
		    sh """
cat <<EOF > .osc-buildpackage.conf
OSC_BUILDPACKAGE_TMP=\"${WORKSPACE}\"
OSC_BUILDPACKAGE_BUILDSCRIPT=\"${WORKSPACE}/osc-buildpackage_buildscript_default\"
EOF
"""
		    sh "osc-buildpkg -v -g -T -P ${env.OBS_TARGET_PROJECT} ${env.OBS_TARGET_REPO} -- --trust-all-projects --build-uid='caller'"
		}
	    }
	    post {
		    always {
			    sh """
				mkdir junit_results
				for file in ${env.OSC_BUILD_ROOT}/usr/src/packages/BUILD/build/tests/whole_dp/*.xml
				do
				xsltproc --output junit_results/\$(basename \$file) vyatta-dataplane/tests/whole_dp/XML_for_JUnit.xsl \$file
				done
			    """

			    junit 'junit_results/*.xml'
		    }
	    }
	}

	stage('Code Stats') {
	    when {expression { env.CHANGE_ID == null }} // Not when this is a Pull Request
	    steps {
		sh 'sloccount --duplicates --wide --details vyatta-dataplane > sloccount.sc'
		sloccountPublish pattern: '**/sloccount.sc'
	    }
	}

	stage('checkpatch') {
	    when {
		allOf {
		    // Only if this is a Pull Request
		    expression { env.CHANGE_ID != null }
		    expression { env.CHANGE_TARGET != null }
		}
	    }
	    steps {
		catchError(buildResult: 'FAILURE', stageResult: 'FAILURE') {
		    dir('vyatta-dataplane') {
		    //TODO: Path to checkpatch.pl should not be hardcoded!
			sh "PATH=~/linux-vyatta/scripts:$PATH ./scripts/checkpatch_wrapper.sh upstream/${env.CHANGE_TARGET} origin/${env.BRANCH_NAME}"
		    }
		}
	    }
	}

	stage('gitlint') {
	    when {
		allOf {
		    // Only if this is a Pull Request
		    expression { env.CHANGE_ID != null }
		    expression { env.CHANGE_TARGET != null }
		}
	    }
	    agent {
		docker { image 'jorisroovers/gitlint'
			args '--entrypoint=""'
			reuseNode true
		}
	    }
	    steps {
		catchError(buildResult: 'FAILURE', stageResult: 'FAILURE') {
		    dir('vyatta-dataplane') {
		        sh "gitlint --commits upstream/${env.CHANGE_TARGET}..origin/${env.BRANCH_NAME}"
		    }
		}
	    }
	}

        stage('Code Static Analysis') {
            steps {
                writeFile file: 'osc-buildpackage_buildscript_default',
                        text: """\
                        export BUILD_ID=\"${BUILD_ID}\"
                        export JENKINS_NODE_COOKIE=\"${JENKINS_NODE_COOKIE}\"
                        export CC=clang-7
                        export CCX=clang++-7
                        meson builddir && cd builddir && ninja
                        run-clang-tidy-7 '.*(?<!pb.cc)(?<!pb-c.c)\$' >& clang-tidy.log
                        sed -i 's|/usr/src/packages/BUILD|${WORKSPACE}/vyatta-dataplane|g' clang-tidy.log
                        """.stripIndent()
                dir('vyatta-dataplane') {
                        writeFile file: '.osc-buildpackage.conf',
                                text: """\
                                OSC_BUILDPACKAGE_TMP=\"${WORKSPACE}\"
                                OSC_BUILDPACKAGE_BUILDSCRIPT=\"${WORKSPACE}/osc-buildpackage_buildscript_default\"
                                """.stripIndent()
                        sh "osc-buildpkg -v -g -T -P ${env.OBS_TARGET_PROJECT} ${env.OBS_TARGET_REPO} -- --trust-all-projects --build-uid='caller' --nochecks --extra-pkgs='clang-tidy-7'"
                }
            }
            post {
                always {
                        archiveArtifacts artifacts: "${env.BUILD_ROOT_RELATIVE}/usr/src/packages/BUILD/builddir/clang-tidy.log"
                        recordIssues enabledForFailure: true,
                                tool: clangTidy(pattern: "${env.BUILD_ROOT_RELATIVE}/usr/src/packages/BUILD/builddir/clang-tidy.log"),
                                sourceDirectory: 'vyatta-dataplane',
                                referenceJobName: "DANOS/vyatta-dataplane/${env.REF_BRANCH}",
                                qualityGates: [[type: 'NEW', threshold: 1, unstable: true]]
                }
            }
        }

    } // stages

    post {
        always {
	    sh 'rm -f *.deb'
	    sh "osc chroot --wipe --force --root ${env.OSC_BUILD_ROOT}"
	    deleteDir()
        }
        success {
            echo 'Winning'
        }
        failure {
            echo 'Argh... something went wrong'
	    emailext (
		subject: "FAILED: Job '${env.JOB_NAME} [${env.BUILD_NUMBER}]'",
		body: """<p>FAILED: Job '${env.JOB_NAME} [${env.BUILD_NUMBER}]':</p>
			 <p>Check console output at "<a href="${env.BUILD_URL}">${env.JOB_NAME} [${env.BUILD_NUMBER}]</a>"</p>""",
		recipientProviders: [culprits()]
	    )
        }
    }
}
